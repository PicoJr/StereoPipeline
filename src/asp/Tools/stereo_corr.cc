// __BEGIN_LICENSE__
//  Copyright (c) 2009-2013, United States Government as represented by the
//  Administrator of the National Aeronautics and Space Administration. All
//  rights reserved.
//
//  The NGT platform is licensed under the Apache License, Version 2.0 (the
//  "License"); you may not use this file except in compliance with the
//  License. You may obtain a copy of the License at
//  http://www.apache.org/licenses/LICENSE-2.0
//
//  Unless required by applicable law or agreed to in writing, software
//  distributed under the License is distributed on an "AS IS" BASIS,
//  WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
//  See the License for the specific language governing permissions and
//  limitations under the License.
// __END_LICENSE__


/// \file stereo_corr.cc
///

#include <vw/Camera/CameraTransform.h>
#include <vw/Camera/PinholeModel.h>
#include <vw/Stereo/CorrelationView.h>
#include <vw/Stereo/CostFunctions.h>
#include <vw/Stereo/DisparityMap.h>
#include <vw/Stereo/StereoModel.h>
#include <vw/Core/StringUtils.h>
#include <vw/InterestPoint/Matcher.h>

#include <asp/Core/DisparityFilter.h>
#include <asp/Core/AffineEpipolar.h>
#include <asp/Core/DemDisparity.h>
#include <asp/Core/InterestPointMatching.h>
#include <asp/Core/LocalAlignment.h>
#include <asp/Sessions/StereoSession.h>
#include <asp/Sessions/StereoSessionPinhole.h>
#include <asp/Tools/stereo.h>

#include <boost/process.hpp>
#include <boost/process/env.hpp>

#include <opencv2/stereo.hpp>

#include <xercesc/util/PlatformUtils.hpp>

using namespace vw;
using namespace vw::stereo;
using namespace asp;
namespace bp = boost::process;

/// Returns the properly cast cost mode type
stereo::CostFunctionType get_cost_mode_value() {
  switch(stereo_settings().cost_mode) {
  case 0: return stereo::ABSOLUTE_DIFFERENCE;
  case 1: return stereo::SQUARED_DIFFERENCE;
  case 2: return stereo::CROSS_CORRELATION;
  case 3: return stereo::CENSUS_TRANSFORM;
  case 4: return stereo::TERNARY_CENSUS_TRANSFORM;
  default: 
    vw_throw(ArgumentErr() << "Unknown value " << stereo_settings().cost_mode
             << " for cost-mode.\n");
  };
}

/// Determine the proper subpixel mode to be used with SGM correlation
SemiGlobalMatcher::SgmSubpixelMode get_sgm_subpixel_mode() {

  switch(stereo_settings().subpixel_mode) {
  case  7: return SemiGlobalMatcher::SUBPIXEL_NONE;
  case  8: return SemiGlobalMatcher::SUBPIXEL_LINEAR;
  case  9: return SemiGlobalMatcher::SUBPIXEL_POLY4;
  case 10: return SemiGlobalMatcher::SUBPIXEL_COSINE;
  case 11: return SemiGlobalMatcher::SUBPIXEL_PARABOLA;
  case 12: return SemiGlobalMatcher::SUBPIXEL_LC_BLEND;
  default: return SemiGlobalMatcher::SUBPIXEL_NONE; // This includes stereo_rfne subpixel modes
  };
}

// Read the search range from D_sub, and scale it to the full image
void read_search_range_from_D_sub(std::string const& d_sub_file, ASPGlobalOptions const& opt){

  // No D_sub is generated or should be used for seed mode 0.
  if (stereo_settings().seed_mode == 0)
    return;

  vw::ImageViewRef<vw::PixelMask<vw::Vector2f>> sub_disp;
  vw::Vector2 upsample_scale;
  
  asp::load_D_sub_and_scale(opt, d_sub_file, sub_disp, upsample_scale);
  
  BBox2 search_range = stereo::get_disparity_range(sub_disp);
  search_range.min() = floor(elem_prod(search_range.min(), upsample_scale));
  search_range.max() = ceil (elem_prod(search_range.max(), upsample_scale));
  stereo_settings().search_range = search_range;

  vw_out() << "\t--> Read search range from D_sub: " << search_range << "\n";
}

/// Produces the low-resolution disparity file D_sub
void produce_lowres_disparity(ASPGlobalOptions & opt) {

  // Set up handles to read the input images
  DiskImageView<vw::uint8> Lmask(opt.out_prefix + "-lMask.tif"),
    Rmask(opt.out_prefix + "-rMask.tif");

  DiskImageView<PixelGray<float> > left_sub (opt.out_prefix+"-L_sub.tif"),
    right_sub(opt.out_prefix+"-R_sub.tif");

  DiskImageView<uint8> left_mask_sub (opt.out_prefix+"-lMask_sub.tif"),
    right_mask_sub(opt.out_prefix+"-rMask_sub.tif");

  Vector2 downsample_scale(double(left_sub.cols()) / double(Lmask.cols()),
                           double(left_sub.rows()) / double(Lmask.rows()));
  double mean_scale = (downsample_scale[0] + downsample_scale[1]) / 2.0;

  // Compute the initial search range in the subsampled image
  BBox2 search_range(floor(elem_prod(downsample_scale,stereo_settings().search_range.min())),
                      ceil (elem_prod(downsample_scale,stereo_settings().search_range.max())));

  vw::stereo::CorrelationAlgorithm stereo_alg
    = asp::stereo_alg_to_num(stereo_settings().stereo_algorithm);

  std::string d_sub_file = opt.out_prefix + "-D_sub.tif";
  std::string spread_file = opt.out_prefix + "-D_sub_spread.tif";

  if (stereo_settings().seed_mode != 3 && fs::exists(spread_file)) {
    // We will recreate D_sub below unless seed_mode is 3, when the work
    // happens in sparse_disp outside this logic. We may or may not recreate
    // D_sub_spread, but in either case wipe the existing one or else
    // it may be a leftover from a previous run with different image
    // sizes, and in that case it will be inconsistent with D_sub
    // we will create now.
    fs::remove(spread_file);
  }
  
  if (stereo_settings().seed_mode == 1) {

    // For D_sub always use a cross-check even if it takes more time.
    // The user-specified xcorr_threshold will be restored at the end.
    int orig_xcorr_threshold = stereo_settings().xcorr_threshold;
    if (orig_xcorr_threshold < 0) 
      stereo_settings().xcorr_threshold = 2;
    
    // Use low-res correlation to get the low-res disparity
    Vector2 expansion(search_range.width(),
                       search_range.height());
    expansion *= stereo_settings().seed_percent_pad / 2.0f;
    // Expand by the user selected amount. Default is 25%.
    search_range.min() -= expansion;
    search_range.max() += expansion;

    //VW_OUT(DebugMessage,"asp") << "D_sub search range: " << search_range << " px\n";
    vw_out() << "D_sub search range: " << search_range << " px\n";
    stereo::CostFunctionType cost_mode = get_cost_mode_value();
    Vector2i kernel_size  = stereo_settings().corr_kernel;
    int corr_timeout      = 5*stereo_settings().corr_timeout; // 5x, so try hard
    const int rm_half_kernel = 5; // Filter kernel size used by CorrelationView
    double seconds_per_op = 0.0;
    if (corr_timeout > 0)
      seconds_per_op = calc_seconds_per_op(cost_mode, left_sub, right_sub, kernel_size);

    SemiGlobalMatcher::SgmSubpixelMode sgm_subpixel_mode = get_sgm_subpixel_mode();
    Vector2i sgm_search_buffer = stereo_settings().sgm_search_buffer;;

    if (stereo_settings().rm_quantile_multiple <= 0.0) {
      // If we can process the entire image in one tile, don't use a collar.
      int collar_size = stereo_settings().sgm_collar_size;
      if ((opt.raster_tile_size[0] > left_sub.cols()) &&
          (opt.raster_tile_size[1] > left_sub.rows())  )
        collar_size = 0;
    
      // TODO: Why the extra filtering step here? 
      // PyramidCorrelationView already performs 1-3 iterations of
      // outlier removal.
      vw_out() << "Writing: " << d_sub_file << std::endl;
      vw::cartography::block_write_gdal_image
        (// Write to disk
         d_sub_file,
         rm_outliers_using_thresh
         (// Throw out individual pixels that are far from any neighbors
          vw::stereo::pyramid_correlate
          (// Compute image correlation using the PyramidCorrelationView class
           left_sub, right_sub,
           left_mask_sub, right_mask_sub,
           vw::stereo::PREFILTER_LOG, stereo_settings().slogW,
           search_range, kernel_size, cost_mode,
           corr_timeout, seconds_per_op,
           stereo_settings().xcorr_threshold, 
           stereo_settings().min_xcorr_level,
           rm_half_kernel,
           stereo_settings().corr_max_levels,
           stereo_alg,
           collar_size, sgm_subpixel_mode, sgm_search_buffer,
           stereo_settings().corr_memory_limit_mb,
           stereo_settings().corr_blob_filter_area*mean_scale,
           stereo_settings().stereo_debug
           ),
          // To do: all these hard-coded values must be replaced with
          // appropriate params from user's stereo.default, for
          // consistency with how disparity is filtered in stereo_fltr,
          // when invoking disparity_cleanup_using_thresh.
          1, 1, // in stereo.default we have 5 5
          // Changing below the hard-coded value from 2.0 to using a
          // param.  The default value will still be 2.0 but is now
          // modifiable. Need to get rid of the 2.0/3.0 factor and
          // study how it affects the result.
          stereo_settings().rm_threshold*2.0/3.0,
          // Another change of hard-coded value to param. Get rid of 0.5/0.6
          // and study the effect.
          (stereo_settings().rm_min_matches/100.0)*0.5/0.6
          ), // End outlier removal arguments
         opt,
         TerminalProgressCallback("asp", "\t--> Low-resolution disparity:")
         );
      // End of giant function call block

      // Restore the user xcorr_threshold
      stereo_settings().xcorr_threshold = orig_xcorr_threshold;

      // Filter D_sub.
      if (stereo_settings().outlier_removal_params[0] < 100.0 &&
          opt.stereo_session != "pinhole"              && // this one has no datum 
          (stereo_settings().alignment_method == "homography" ||
           stereo_settings().alignment_method == "affineepipolar" ||
           stereo_settings().alignment_method == "local_epipolar")) {
        
        boost::shared_ptr<camera::CameraModel> left_camera_model, right_camera_model;
        opt.session->camera_models(left_camera_model, right_camera_model);
        const bool use_sphere_for_datum = false;
        vw::cartography::Datum datum = opt.session->get_datum(left_camera_model.get(),
                                                              use_sphere_for_datum);
        asp::filter_D_sub(opt, left_camera_model, right_camera_model, datum, d_sub_file,
                          stereo_settings().outlier_removal_params);
      }
      
    } else {
      // Use quantile based filtering. This filter needs to be profiled to improve its speed.
      
      // Compute image correlation using the PyramidCorrelationView class
      ImageView< PixelMask<Vector2f> > disp_image
        = vw::stereo::pyramid_correlate
        (left_sub, right_sub,
         left_mask_sub, right_mask_sub,
         vw::stereo::PREFILTER_LOG, stereo_settings().slogW,
         search_range, kernel_size, cost_mode,
         corr_timeout, seconds_per_op,
         stereo_settings().xcorr_threshold, 
         stereo_settings().min_xcorr_level,
         rm_half_kernel,
         stereo_settings().corr_max_levels,
         stereo_alg, 
         0, // No collar here, the entire image is written at once.
         sgm_subpixel_mode, sgm_search_buffer, stereo_settings().corr_memory_limit_mb,
         0, // Don't combine blob filtering with quantile filtering
         stereo_settings().stereo_debug
         );
      
      vw_out() << "Writing: " << d_sub_file << std::endl;
      vw::cartography::write_gdal_image
        (// Write to disk while removing outliers
         d_sub_file,
         rm_outliers_using_quantiles
         (// Throw out individual pixels that are far from any neighbors
          disp_image,
          stereo_settings().rm_quantile_percentile, stereo_settings().rm_quantile_multiple
          ), opt,
         TerminalProgressCallback("asp", "\t--> Low-resolution disparity:")
         );
    }
    
  } else if (stereo_settings().seed_mode == 2) {
    // Use a DEM to get the low-res disparity
    boost::shared_ptr<camera::CameraModel> left_camera_model, right_camera_model;
    opt.session->camera_models(left_camera_model, right_camera_model);
    produce_dem_disparity(opt, left_camera_model, right_camera_model, opt.session->name());
  }else if (stereo_settings().seed_mode == 3) {
    // D_sub is already generated by now by sparse_disp
  }

  read_search_range_from_D_sub(d_sub_file, opt); // TODO: We already call this when needed!
} // End produce_lowres_disparity

/// Adjust IP lists if alignment matrices are present.
double adjust_ip_for_align_matrix(std::string               const& out_prefix,
                                  std::vector<ip::InterestPoint>      & ip_left,
                                  std::vector<ip::InterestPoint>      & ip_right,
                                  double                    const  ip_scale) {

  // Check for alignment files
  bool left_align  = fs::exists(out_prefix+"-align-L.exr");
  bool right_align = fs::exists(out_prefix+"-align-R.exr");
  if (!left_align && !right_align)
    return ip_scale; // No alignment files -> Nothing to do.

  // Load alignment matrices
  Matrix<double> align_left_matrix  = math::identity_matrix<3>();
  Matrix<double> align_right_matrix = math::identity_matrix<3>();
  if (left_align)
    read_matrix(align_left_matrix, out_prefix + "-align-L.exr");
  if (right_align)
    read_matrix(align_right_matrix, out_prefix + "-align-R.exr");

  // Loop through all the IP we found
  for (size_t i = 0; i < ip_left.size(); i++) {
    // Apply the alignment transforms to the recorded IP
    Vector3 l = align_left_matrix  * Vector3(ip_left [i].x, ip_left [i].y, 1);
    Vector3 r = align_right_matrix * Vector3(ip_right[i].x, ip_right[i].y, 1);

    // Normalize the coordinates, but don't divide by 0
    if (l[2] == 0 || r[2] == 0) 
      continue;
    l /= l[2];
    r /= r[2];

    ip_left [i].x = l[0];
    ip_left [i].y = l[1];
    ip_left [i].ix = l[0];
    ip_left [i].iy = l[1];
    
    ip_right[i].x = r[0];
    ip_right[i].y = r[1];
    ip_right[i].ix = r[0];
    ip_right[i].iy = r[1];
  }
  return 1.0; // If alignment files are present they take care of the scaling.
              
} // End adjust_ip_for_align_matrix


/// Adjust IP lists if epipolar alignment was applied after the IP were created.
/// - Currently this condition can only happen if an IP file is inserted into the run
///   folder from another source such as bundle adjust!
/// - Returns true if any change was made to the interest points.
bool adjust_ip_for_epipolar_transform(ASPGlobalOptions          const& opt,
                                      std::string               const& match_file,
                                      std::vector<ip::InterestPoint>      & ip_left,
                                      std::vector<ip::InterestPoint>      & ip_right) {

  bool usePinholeEpipolar = ((stereo_settings().alignment_method == "epipolar") &&
                             (opt.session->name() == "pinhole" ||
                              opt.session->name() == "nadirpinhole"));
  
  if (!usePinholeEpipolar) 
    return false;
  
  // This function does nothing if we are not using epipolar alignment,
  //  or if the IP were found using one of the aligned images.
  const std::string sub_match_file
    = vw::ip::match_filename(opt.out_prefix, "L_sub.tif", "R_sub.tif");
  const std::string aligned_match_file
    = vw::ip::match_filename(opt.out_prefix, "L.tif", "R.tif");

  if ((stereo_settings().alignment_method != "epipolar") ||
      (match_file == sub_match_file) || (match_file == aligned_match_file)) 
    return false;

  vw_out() << "Applying epipolar adjustment to input IP match file...\n";

  // Get the transforms from the input image pixels to the epipolar aligned image pixels
  // - Need to cast the session pointer to Pinhole type to access the function we need.
  StereoSessionPinhole* pinPtr = dynamic_cast<StereoSessionPinhole*>(opt.session.get());
  if (pinPtr == NULL) 
    vw_throw(ArgumentErr() << "Expected a pinhole camera.\n");
  StereoSession::tx_type trans_left, trans_right;
  pinPtr->pinhole_cam_trans(trans_left, trans_right);

  // Apply the transforms to all the IP we found
  for (size_t i = 0; i < ip_left.size(); i++) {

    Vector2 ip_in_left (ip_left [i].x, ip_left [i].y);
    Vector2 ip_in_right(ip_right[i].x, ip_right[i].y);

    Vector2 ip_out_left  = trans_left->forward(ip_in_left);
    Vector2 ip_out_right = trans_right->forward(ip_in_right);

    ip_left [i].x = ip_out_left [0]; // Store transformed points
    ip_left [i].y = ip_out_left [1];
    ip_right[i].x = ip_out_right[0];
    ip_right[i].y = ip_out_right[1];
  }

  return true;
} // End adjust_ip_for_epipolar_transform

/// Detect IP in the sub images or the original images if they are not too large.
/// - Usually an IP file is written in stereo_pprc, but for some input scenarios
///   this function will need to be used to generate them here.
/// - The input match file path can be changed depending on what exists on disk.
/// - Returns the scale from the image used for IP to the full size image.
/// - The binary interest point file will be written to disk.
double compute_ip(ASPGlobalOptions & opt, std::string & match_filename) {

  vw_out() << "\t    * Loading images for IP detection.\n";

  double ip_scale = 1.0;

  const std::string left_aligned_image_file  = opt.out_prefix + "-L.tif";
  const std::string right_aligned_image_file = opt.out_prefix + "-R.tif";
  const std::string left_image_path_sub      = opt.out_prefix + "-L_sub.tif";
  const std::string right_image_path_sub     = opt.out_prefix + "-R_sub.tif";

  const std::string unaligned_match_file
    = vw::ip::match_filename(opt.out_prefix,
                             opt.session->left_cropped_image(),
                             opt.session->right_cropped_image());

  const std::string aligned_match_file     
    = vw::ip::match_filename(opt.out_prefix, "L.tif", "R.tif");
  const std::string sub_match_file
    = vw::ip::match_filename(opt.out_prefix, "L_sub.tif", "R_sub.tif");

  // Make sure the match file is newer than these files
  std::vector<std::string> ref_list;
  ref_list.push_back(opt.session->left_cropped_image());
  ref_list.push_back(opt.session->right_cropped_image());
  if (fs::exists(opt.cam_file1))
    ref_list.push_back(opt.cam_file1);
  if (fs::exists(opt.cam_file2))
    ref_list.push_back(opt.cam_file2);

  // Try the unaligned match file first
  if (fs::exists(unaligned_match_file) && is_latest_timestamp(unaligned_match_file, ref_list)) {
    vw_out() << "Cached IP match file found: " << unaligned_match_file << std::endl;
    match_filename = unaligned_match_file;
    return ip_scale;
  }

  // Then tried the aligned match file.
  // TODO(oalexan1): This heuristics is fragile.
  // This should happen only for alignment method none or epipolar, but need to check
  if (fs::exists(aligned_match_file) && is_latest_timestamp(aligned_match_file, ref_list)) {
    vw_out() << "Cached IP match file found: " << aligned_match_file << std::endl;
    match_filename = aligned_match_file;
    return ip_scale;
  }

  // Now try the aligned match file
  std::string left_image_path  = left_aligned_image_file;
  std::string right_image_path = right_aligned_image_file;

  match_filename = aligned_match_file;

  vw_out() << "No IP file found, computing IP now.\n";
  
  // Load the images
  boost::shared_ptr<DiskImageResource> left_rsrc (DiskImageResourcePtr(left_image_path)),
    right_rsrc(DiskImageResourcePtr(right_image_path));

  std::string left_ip_filename  = ip::ip_filename(opt.out_prefix, left_image_path);
  std::string right_ip_filename = ip::ip_filename(opt.out_prefix, right_image_path);

  // Read the no-data values written to disk previously when
  // the normalized left and right sub-images were created.
  float left_nodata_value  = std::numeric_limits<float>::quiet_NaN();
  float right_nodata_value = std::numeric_limits<float>::quiet_NaN();
  if (left_rsrc->has_nodata_read ()) left_nodata_value  = left_rsrc->nodata_read ();
  if (right_rsrc->has_nodata_read()) right_nodata_value = right_rsrc->nodata_read();
  
  // These images should be small enough to fit in memory
  ImageView<float> left_image  = DiskImageView<float>(left_rsrc);
  ImageView<float> right_image = DiskImageView<float>(right_rsrc);

  // No interest point operations have been performed before
  vw_out() << "\t    * Detecting interest points\n";

  bool success = false;
  
  // TODO: Depending on alignment method, we can tailor the IP filtering strategy.
  double thresh_factor = stereo_settings().ip_inlier_factor; // 1/15 by default
  
  // This range is extra large to handle elevation differences.
  const int inlier_threshold = 200*(15.0*thresh_factor);  // 200 by default
  
  success = asp::homography_ip_matching(left_image, right_image,
                                        stereo_settings().ip_per_tile,
                                        inlier_threshold, match_filename,
                                        left_ip_filename, right_ip_filename,
                                        left_nodata_value, right_nodata_value);

  if (!success)
    vw_throw(ArgumentErr() << "Could not find interest points.\n");

  return ip_scale;
}

BBox2 get_search_range_from_ip_hists(vw::math::Histogram const& hist_x,
                                     vw::math::Histogram const& hist_y,
                                     double edge_discard_percentile = 0.05) {

  const double min_percentile = edge_discard_percentile;
  const double max_percentile = 1.0 - edge_discard_percentile;
  double search_scale = 2.0;

  vw_out() << "Filtering IP using box-and-whisker plot. "
           << "Using the values corresponding to percentiles " << min_percentile * 100.0 << " and "
           << max_percentile * 100.0 << ", with a factor of "
           << search_scale << " to get the whiskers.\n";
    
  const Vector2 FORCED_EXPANSION = Vector2(30,2); // Must expand range by at least this much
  size_t min_bin_x = hist_x.get_percentile(min_percentile);
  size_t min_bin_y = hist_y.get_percentile(min_percentile);
  size_t max_bin_x = hist_x.get_percentile(max_percentile);
  size_t max_bin_y = hist_y.get_percentile(max_percentile);
  Vector2 search_min(hist_x.get_bin_center(min_bin_x), hist_y.get_bin_center(min_bin_y));
  Vector2 search_max(hist_x.get_bin_center(max_bin_x), hist_y.get_bin_center(max_bin_y));
  Vector2 search_center = (search_max + search_min) / 2.0;
  Vector2 d_min = search_min - search_center; // TODO: Make into a bbox function!
  Vector2 d_max = search_max - search_center;

  vw_out(InfoMessage,"asp") << "Range based on percentiles: " << BBox2(d_min, d_max) << std::endl;
  
  // Enforce a minimum expansion on the search range in each direction
  Vector2 min_expand = d_min*search_scale;
  Vector2 max_expand = d_max*search_scale;

  for (int i=0; i<2; ++i) {
    if (min_expand[i] > -1*FORCED_EXPANSION[i])
      min_expand[i] = -1*FORCED_EXPANSION[i];
    if (max_expand[i] < FORCED_EXPANSION[i])
      max_expand[i] = FORCED_EXPANSION[i];
  }

  
  search_min = search_center + min_expand;
  search_max = search_center + max_expand;

  Vector2 search_minI(floor(search_min[0]), floor(search_min[1])); // Round outwards
  Vector2 search_maxI(ceil (search_max[0]), ceil (search_max[1]));
  /*
  // Debug code to print all the points
  for (size_t i = 0; i < matched_ip1.size(); i++) {
  Vector2f diff(i_scale * (matched_ip2[i].x - matched_ip1[i].x), 
  i_scale * (matched_ip2[i].y - matched_ip1[i].y));
  //Vector2f diff(matched_ip2[i].x - matched_ip1[i].x, 
  //              matched_ip2[i].y - matched_ip1[i].y);
  vw_out(InfoMessage,"asp") << matched_ip1[i].x <<", "<<matched_ip1[i].y 
  << " <> " 
  << matched_ip2[i].x <<", "<<matched_ip2[i].y 
  << " DIFF " << diff << std::endl;
  }
  */
  
  //vw_out(InfoMessage,"asp") << "i_scale is : "       << i_scale << std::endl;
  
  return BBox2(search_minI, search_maxI);
}
  
/// Use existing interest points to compute a search range
/// - This function could use improvement!
/// - Should it be used in all cases?
BBox2 approximate_search_range(ASPGlobalOptions & opt, 
                                double ip_scale, std::string const& match_filename) {

  vw_out() << "\t--> Using interest points to determine search window.\n";
  std::vector<ip::InterestPoint> in_ip1, in_ip2, matched_ip1, matched_ip2;

  // The interest points must have been created outside this function
  if (!fs::exists(match_filename))
    vw_throw(ArgumentErr() << "Missing IP file: " << match_filename);
  
  vw_out() << "\t    * Loading match file: " << match_filename << "\n";
  ip::read_binary_match_file(match_filename, in_ip1, in_ip2);
  
  // TODO(oalexan1): Consolidate IP adjustment.
  // TODO(oalexan1): This logic is messed up. We __know__ from
  // stereo_settings() what alignment method is being used and what
  // scale we are at, there is no need to try to read various and
  // likely old files from disk to infer that. You can get the wrong
  // answer.
  
  // Handle alignment matrices if they are present
  // - Scale is reset to 1.0 if alignment matrices are present.
  ip_scale = adjust_ip_for_align_matrix(opt.out_prefix, in_ip1, in_ip2, ip_scale);
  vw_out() << "\t    * IP computed at scale: " << ip_scale << "\n";

  // TODO(oalexan1): Remove the scale from everywhere, as it is always 1.
  float i_scale = 1.0/ip_scale;

  // Adjust the IP if they came from input images and these images are epipolar aligned
  // This can be very useful if the ip come from outside, such as bundle adjustment.
  // TODO(oalexan1): This should be exclusive with adjust_ip_for_align_matrix.
  adjust_ip_for_epipolar_transform(opt, match_filename, in_ip1, in_ip2);

  // Filter out IPs which fall outside the specified elevation range
  boost::shared_ptr<camera::CameraModel> left_camera_model, right_camera_model;
  opt.session->camera_models(left_camera_model, right_camera_model);
  cartography::Datum datum = opt.session->get_datum(left_camera_model.get(), false);

  // Filter out IPs which fall outside the specified elevation and lonlat range
  // TODO(oalexan1): Study this. Don't do this with cropped input images!!!!!
  size_t num_left = asp::filter_ip_by_lonlat_and_elevation(left_camera_model.get(),
                                                           right_camera_model.get(),
                                                           datum, in_ip1, in_ip2, ip_scale,
                                                           stereo_settings().elevation_limit,
                                                           stereo_settings().lon_lat_limit,
                                                           matched_ip1, matched_ip2);

  // If the user set this, filter by disparity of ip.
  // TODO: This kind of logic is present below one more time, at
  // get_search_range_from_ip_hists() where a factor of 2 is used!
  // This logic better be integrated together!
  // TODO(oalexan1): Integrate this with existing logic.
  Vector2 disp_params = stereo_settings().remove_outliers_by_disp_params;
  if (disp_params[0] < 100.0) // not enabled by default
    asp::filter_ip_by_disparity(disp_params[0], disp_params[1], matched_ip1, matched_ip2); 
  
  // Quit if we don't have the requested number of IP.
  if (static_cast<int>(num_left) < stereo_settings().min_num_ip)
    vw_throw(ArgumentErr() << "Number of IPs left after filtering is " << num_left
             << " which is less than the required amount of " 
             << stereo_settings().min_num_ip << ", aborting stereo_corr. "
             << "A solution may be to remove the run directory and restart stereo "
             << "while setting --ip-per-tile 200 or some other larger number. "
             << "Otherwise decrease --min-num-ip to accept these matches.\n");
  
  // Find search window based on interest point matches
  size_t num_ip = matched_ip1.size();
  vw_out(InfoMessage, "asp") << "Estimating search range with " 
                             << num_ip << " interest points.\n";

  // Record the disparities for each point pair
  const double BIG_NUM   =  99999999;
  const double SMALL_NUM = -99999999;
  std::vector<double> dx, dy;
  double min_dx = BIG_NUM, max_dx = SMALL_NUM,
    min_dy = BIG_NUM, max_dy = SMALL_NUM;
  for (size_t i = 0; i < num_ip; i++) {
    double diffX = i_scale * (matched_ip2[i].x - matched_ip1[i].x);
    double diffY = i_scale * (matched_ip2[i].y - matched_ip1[i].y);      
    dx.push_back(diffX);
    dy.push_back(diffY);
    if (diffX < min_dx) min_dx = diffX;
    if (diffY < min_dy) min_dy = diffY;
    if (diffX > max_dx) max_dx = diffX;
    if (diffY > max_dy) max_dy = diffY;
  }

  vw_out(InfoMessage,"asp") << "Initial search range: " 
                            << BBox2(Vector2(min_dx,min_dy), Vector2(max_dx,max_dy)) << std::endl;

  const int MAX_SEARCH_WIDTH = 4000; // Try to avoid searching this width
  const int MIN_SEARCH_WIDTH = 200;  // Under this width don't filter IP.
  const Vector2i MINIMAL_EXPAND(10,1);

  // If the input search range is small just expand it a bit and
  //  return without doing any filtering.  
  if (max_dx-min_dx <= MIN_SEARCH_WIDTH) {
    BBox2 search_range(Vector2i(min_dx,min_dy),Vector2i(max_dx,max_dy));
    search_range.min() -= MINIMAL_EXPAND; // BBox2.expand() function does not always work!!!!
    search_range.max() += MINIMAL_EXPAND;
    vw_out(InfoMessage,"asp") << "Using expanded search range: " 
                              << search_range << std::endl;
    return search_range;
  }
  
  // Compute histograms
  const int NUM_BINS = 1000000; // Accuracy is important with scaled pixels
  vw::math::Histogram hist_x(NUM_BINS, min_dx, max_dx);
  vw::math::Histogram hist_y(NUM_BINS, min_dy, max_dy);
  for (size_t i = 0; i < dx.size(); ++i){
    hist_x.add_value(dx[i]);
    hist_y.add_value(dy[i]);
  }

  //printf("min x,y = %lf, %lf, max x,y = %lf, %lf\n", min_dx, min_dy, max_dx, max_dy);
  //for (int i=0; i<NUM_BINS; ++i) {
  //  printf("%d => X: %lf: %lf,   Y:  %lf: %lf\n", i, centers_x[i], hist_x[i], centers_y[i], hist_y[i]);
  //}

  const double PERCENTILE_CUTOFF     = 0.05; // Gradually increase the filtering
  const double PERCENTILE_CUTOFF_INC = 0.05; //  until the search width is reasonable.
  const double MAX_PERCENTILE_CUTOFF = 0.201;

  double current_percentile_cutoff = PERCENTILE_CUTOFF;
  int search_width = MAX_SEARCH_WIDTH + 1;
  BBox2 search_range;
  while (true) {
    search_range = get_search_range_from_ip_hists(hist_x, hist_y, current_percentile_cutoff);
    vw_out() << "Computed search range: " << search_range << std::endl;
    search_width = search_range.width();
    
    // Increase the percentile cutoff in case we need to filter out more IP
    current_percentile_cutoff += PERCENTILE_CUTOFF_INC;
    if (current_percentile_cutoff > MAX_PERCENTILE_CUTOFF) {
      if (search_width < MAX_SEARCH_WIDTH)
        vw_out() << "Exceeded maximum filter cutoff of " << MAX_PERCENTILE_CUTOFF
                 << ", keeping current search range\n";
      break; // No more filtering is possible, exit the loop.
    }
      
    if (search_width < MAX_SEARCH_WIDTH)
      break; // Happy with search range, exit the loop.
    else
      vw_out() << "Search width of " << search_width << " is greater than desired limit of "
               << MAX_SEARCH_WIDTH << ", retrying with more aggressive IP filter\n";
  } // End search range determination loop

  
  // Prevent any dimension from being length zero,
  //  otherwise future parts to ASP will fail.
  // TODO: Fix ASP and SGM handling of small boxes!
  //       - Currently code has a minimum search height of 5!
  if (search_range.empty())
    vw_throw(ArgumentErr() << "Computed an empty search range!");
  
  return search_range;
} // End function approximate_search_range


/// The first step of correlation computation.
void lowres_correlation(ASPGlobalOptions & opt) {

  vw_out() << "\n[ " << current_posix_time_string()
           << " ] : Stage 1 --> LOW-RESOLUTION CORRELATION\n";

  // Working out search range if need be
  if (stereo_settings().is_search_defined()) {
    vw_out() << "\t--> Using user-defined search range.\n";

    // Update user provided search range based on input crops
    bool crop_left  = (stereo_settings().left_image_crop_win  != BBox2i(0, 0, 0, 0));
    bool crop_right = (stereo_settings().right_image_crop_win != BBox2i(0, 0, 0, 0));
    if (crop_left && !crop_right)
      stereo_settings().search_range += stereo_settings().left_image_crop_win.min();
    if (!crop_left && crop_right)
      stereo_settings().search_range -= stereo_settings().right_image_crop_win.min();

  }else if (stereo_settings().seed_mode == 2){
    // Do nothing as we will compute the search range based on D_sub
  }else if (stereo_settings().seed_mode == 3){
    // Do nothing as low-res disparity (D_sub) is already provided by sparse_disp
  } else { // Regular seed mode

    // TODO(oalexan1): All ip matching should happen in stereo_pprc for consistency.
    
    // Load IP from disk if they exist, or else compute them. 
    std::string match_filename;
    double ip_scale;
    ip_scale = compute_ip(opt, match_filename);

    // This function applies filtering to find good points
    stereo_settings().search_range = approximate_search_range(opt, ip_scale, match_filename);

    vw_out() << "\t--> Detected search range: " << stereo_settings().search_range << "\n";
  } // End of case where we had to calculate the search range

  // If the user specified a search range limit, apply it here.
  if ((stereo_settings().search_range_limit.min() != Vector2i()) || 
      (stereo_settings().search_range_limit.max() != Vector2i())  ) {     
    stereo_settings().search_range.crop(stereo_settings().search_range_limit);
    vw_out() << "\t--> Detected search range constrained to: "
             << stereo_settings().search_range << "\n";
  }

  // At this point stereo_settings().search_range is populated

  DiskImageView<vw::uint8> Lmask(opt.out_prefix + "-lMask.tif"),
    Rmask(opt.out_prefix + "-rMask.tif");

  // Performing disparity on sub images
  if (stereo_settings().seed_mode > 0) {

    // Reuse prior existing D_sub if it exists, unless we
    // are cropping the images each time, when D_sub must
    // be computed anew each time.
    bool crop_left  = (stereo_settings().left_image_crop_win  != BBox2i(0, 0, 0, 0));
    bool crop_right = (stereo_settings().right_image_crop_win != BBox2i(0, 0, 0, 0));
    
    std::string sub_disp_file = opt.out_prefix + "-D_sub.tif";

    // Also need to rebuild if the inputs changed after the mask files were produced.
    bool inputs_changed = (!is_latest_timestamp(sub_disp_file, opt.in_file1,  opt.in_file2,
                                                opt.cam_file1, opt.cam_file2));

    bool rebuild = crop_left || crop_right || inputs_changed;

    try {
      vw_log().console_log().rule_set().add_rule(-1,"fileio");
      DiskImageView<PixelMask<Vector2f> > test(sub_disp_file);
      vw_settings().reload_config();
    } catch (vw::IOErr const& e) {
      vw_settings().reload_config();
      rebuild = true;
    } catch (vw::ArgumentErr const& e) {
      // Throws on a corrupted file.
      vw_settings().reload_config();
      rebuild = true;
    }

    if (rebuild) {
      // It will be rebuilt except for seed-mode 3 when sparse_disp takes care of it.
      produce_lowres_disparity(opt);
    } else {
      vw_out() << "\t--> Using cached low-resolution disparity: " << sub_disp_file << "\n";
    }
  }

  vw_out() << "\n[ " << current_posix_time_string()
           << " ] : LOW-RESOLUTION CORRELATION FINISHED\n";
} // End lowres_correlation

/// This correlator takes a low resolution disparity image as an input
/// so that it may narrow its search range for each tile that is processed.
class SeededCorrelatorView : public ImageViewBase<SeededCorrelatorView> {
  ImageViewRef<PixelGray<float> >   m_left_image;
  ImageViewRef<PixelGray<float> >   m_right_image;
  ImageViewRef<vw::uint8> m_left_mask;
  ImageViewRef<vw::uint8> m_right_mask;
  ImageViewRef<PixelMask<Vector2f> > m_sub_disp;
  ImageViewRef<PixelMask<Vector2i> > m_sub_disp_spread;

  // Settings
  Vector2  m_upscale_factor;
  BBox2i   m_seed_bbox;
  Vector2i m_kernel_size;
  stereo::CostFunctionType m_cost_mode;
  int      m_corr_timeout;
  double   m_seconds_per_op;

public:

  // Set these input types here instead of making them template arguments
  typedef ImageViewRef<PixelGray<float> >   ImageType;
  typedef ImageViewRef<vw::uint8>           MaskType;
  typedef ImageViewRef<PixelMask<Vector2f> > DispSeedImageType;
  typedef ImageViewRef<PixelMask<Vector2i> > SpreadImageType;
  typedef ImageType::pixel_type InputPixelType;

  SeededCorrelatorView(ImageType             const& left_image,
                       ImageType             const& right_image,
                       MaskType              const& left_mask,
                       MaskType              const& right_mask,
                       DispSeedImageType     const& sub_disp,
                       SpreadImageType       const& sub_disp_spread,
                       Vector2i const& kernel_size,
                       stereo::CostFunctionType cost_mode,
                       int corr_timeout, double seconds_per_op) :
    m_left_image(left_image.impl()), m_right_image(right_image.impl()),
    m_left_mask (left_mask.impl ()), m_right_mask (right_mask.impl ()),
    m_sub_disp(sub_disp.impl()), m_sub_disp_spread(sub_disp_spread.impl()),
    m_kernel_size(kernel_size),  m_cost_mode(cost_mode),
    m_corr_timeout(corr_timeout), m_seconds_per_op(seconds_per_op){
    m_upscale_factor[0] = double(m_left_image.cols()) / m_sub_disp.cols();
    m_upscale_factor[1] = double(m_left_image.rows()) / m_sub_disp.rows();
    m_seed_bbox = bounding_box(m_sub_disp);
  }

  // Image View interface
  typedef PixelMask<Vector2f> pixel_type;
  typedef pixel_type          result_type;
  typedef ProceduralPixelAccessor<SeededCorrelatorView> pixel_accessor;

  inline int32 cols  () const { return m_left_image.cols(); }
  inline int32 rows  () const { return m_left_image.rows(); }
  inline int32 planes() const { return 1; }

  inline pixel_accessor origin() const { return pixel_accessor(*this, 0, 0); }

  inline pixel_type operator()(double /*i*/, double /*j*/, int32 /*p*/ = 0) const {
    vw_throw(NoImplErr() << "SeededCorrelatorView::operator()(...) is not implemented");
    return pixel_type();
  }

  /// Does the work
  typedef CropView<ImageView<pixel_type> > prerasterize_type;
  inline prerasterize_type prerasterize(BBox2i const& bbox) const {

    Matrix<double> lowres_hom  = math::identity_matrix<3>();
    Matrix<double> fullres_hom = math::identity_matrix<3>();
    ImageViewRef<InputPixelType> right_trans_img;
    ImageViewRef<vw::uint8     > right_trans_mask;

    bool do_round = true; // round integer disparities after transform

    vw::stereo::CorrelationAlgorithm stereo_alg
      = asp::stereo_alg_to_num(stereo_settings().stereo_algorithm);
    
    // User strategies
    BBox2 local_search_range;
    if (stereo_settings().seed_mode > 0) {

      // The low-res version of bbox
      BBox2i seed_bbox(elem_quot(bbox.min(), m_upscale_factor),
                       elem_quot(bbox.max(), m_upscale_factor));
      seed_bbox.expand(1);
      seed_bbox.crop(m_seed_bbox);
      // Get the disparity range in d_sub corresponding to this tile.
      VW_OUT(DebugMessage, "stereo") << "\nGetting disparity range for : " << seed_bbox << "\n";
      DispSeedImageType disparity_in_box = crop(m_sub_disp, seed_bbox);

      local_search_range = stereo::get_disparity_range(disparity_in_box);

      bool has_sub_disp_spread = (m_sub_disp_spread.cols() != 0 &&
                                  m_sub_disp_spread.rows() != 0);
      // Sanity check: If m_sub_disp_spread was provided, it better have the same size as sub_disp.
      if (has_sub_disp_spread &&
          m_sub_disp_spread.cols() != m_sub_disp.cols() &&
          m_sub_disp_spread.rows() != m_sub_disp.rows()){
        vw_throw(ArgumentErr() << "stereo_corr: D_sub and D_sub_spread must have equal sizes.\n");
      }

      if (has_sub_disp_spread){
        // Expand the disparity range by m_sub_disp_spread.
        SpreadImageType spread_in_box = crop(m_sub_disp_spread, seed_bbox);

        BBox2 spread = stereo::get_disparity_range(spread_in_box);
        local_search_range.min() -= spread.max();
        local_search_range.max() += spread.max();
      } //endif has_sub_disp_spread

      local_search_range = grow_bbox_to_int(local_search_range);
      // Expand local_search_range by 1. This is necessary since
      // m_sub_disp is integer-valued, and perhaps the search
      // range was supposed to be a fraction of integer bigger.
      local_search_range.expand(1);

      // Scale the search range to full-resolution
      local_search_range.min() = floor(elem_prod(local_search_range.min(),m_upscale_factor));
      local_search_range.max() = ceil (elem_prod(local_search_range.max(),m_upscale_factor));

      // If the user specified a search range limit, apply it here.
      if ((stereo_settings().search_range_limit.min() != Vector2i()) || 
          (stereo_settings().search_range_limit.max() != Vector2i())  ) {     
        local_search_range.crop(stereo_settings().search_range_limit);
        vw_out() << "\t--> Local search range constrained to: " << local_search_range << "\n";
      }

      VW_OUT(DebugMessage, "stereo") << "SeededCorrelatorView("
                                     << bbox << ") local search range "
                                     << local_search_range << " vs "
                                     << stereo_settings().search_range << "\n";

    } else{ // seed mode == 0
      local_search_range = stereo_settings().search_range;
      VW_OUT(DebugMessage,"stereo") << "Searching with " << stereo_settings().search_range << "\n";
    }

    SemiGlobalMatcher::SgmSubpixelMode sgm_subpixel_mode = get_sgm_subpixel_mode();
    Vector2i sgm_search_buffer = stereo_settings().sgm_search_buffer;

    // Now we are ready to actually perform correlation
    const int rm_half_kernel = 5; // Filter kernel size used by CorrelationView
    typedef vw::stereo::PyramidCorrelationView<ImageType, ImageType, MaskType, MaskType > CorrView;
    CorrView corr_view(m_left_image,   m_right_image,
                       m_left_mask,    m_right_mask,
                       static_cast<vw::stereo::PrefilterModeType>(stereo_settings().pre_filter_mode),
                       stereo_settings().slogW,
                       local_search_range,
                       m_kernel_size,  m_cost_mode,
                       m_corr_timeout, m_seconds_per_op,
                       stereo_settings().xcorr_threshold,
                       stereo_settings().min_xcorr_level,
                       rm_half_kernel,
                       stereo_settings().corr_max_levels,
                       stereo_alg, 
                       stereo_settings().sgm_collar_size,
                       sgm_subpixel_mode, sgm_search_buffer,
                       stereo_settings().corr_memory_limit_mb,
                       stereo_settings().corr_blob_filter_area,
                       stereo_settings().stereo_debug);
    return corr_view.prerasterize(bbox);
    
  } // End function prerasterize_helper

  template <class DestT>
  inline void rasterize(DestT const& dest, BBox2i bbox) const {
    vw::rasterize(prerasterize(bbox), dest, bbox);
  }
}; // End class SeededCorrelatorView


/// Stereo correlation function using ASP's block-matching and MGM/SGM
/// algorithms which can handle a 2D disparity.
void stereo_correlation_2D(ASPGlobalOptions& opt) {

  // The first thing we will do is compute the low-resolution correlation.

  // Note that even when we are told to skip low-resolution correlation,
  // we must still go through the motions when seed_mode is 0, to be
  // able to get a search range, even though we don't write D_sub then.
  if (!stereo_settings().skip_low_res_disparity_comp || stereo_settings().seed_mode == 0)
    lowres_correlation(opt);

  if (stereo_settings().compute_low_res_disparity_only) 
    return; // Just computed the low-res disparity, so quit.

  std::string d_sub_file  = opt.out_prefix + "-D_sub.tif";
  std::string spread_file = opt.out_prefix + "-D_sub_spread.tif";

  read_search_range_from_D_sub(d_sub_file, opt);

  // If the user specified a search range limit, apply it here.
  if ((stereo_settings().search_range_limit.min() != Vector2i()) || 
      (stereo_settings().search_range_limit.max() != Vector2i())  ) {     
    stereo_settings().search_range.crop(stereo_settings().search_range_limit);
    vw_out() << "\t--> Detected search range constrained to: "
             << stereo_settings().search_range << "\n";
  }

  // Provide the user with some feedback of what we are actually going to use.
  vw_out()   << "\t--------------------------------------------------\n";
  vw_out()   << "\t   Kernel size:    " << stereo_settings().corr_kernel << std::endl;
  if (stereo_settings().seed_mode > 0)
    vw_out() << "\t   Refined search: " << stereo_settings().search_range << std::endl;
  else
    vw_out() << "\t   Search range:   " << stereo_settings().search_range << std::endl;
  vw_out()   << "\t   Cost mode:      " << stereo_settings().cost_mode << std::endl;
  vw_out(DebugMessage) << "\t   XCorr threshold: " << stereo_settings().xcorr_threshold << std::endl;
  vw_out(DebugMessage) << "\t   Prefilter:       " << stereo_settings().pre_filter_mode << std::endl;
  vw_out(DebugMessage) << "\t   Prefilter size:  " << stereo_settings().slogW << std::endl;
  vw_out() << "\t--------------------------------------------------\n";

  // Load up for the actual native resolution processing

  std::string left_image_file = opt.out_prefix + "-L.tif";
  std::string right_image_file = opt.out_prefix + "-R.tif";
  
  boost::shared_ptr<DiskImageResource>
    left_rsrc (vw::DiskImageResourcePtr(left_image_file)),
    right_rsrc(vw::DiskImageResourcePtr(right_image_file));

  // Load the normalized images.
  DiskImageView<PixelGray<float> > left_disk_image (left_rsrc ),
                                   right_disk_image(right_rsrc);
  
  DiskImageView<vw::uint8> Lmask(opt.out_prefix + "-lMask.tif"),
    Rmask(opt.out_prefix + "-rMask.tif");
  ImageViewRef<PixelMask<Vector2f> > sub_disp;
  
  if (stereo_settings().seed_mode > 0) {
    if (!load_D_sub(d_sub_file, sub_disp)) {
      std::string msg = "Could not read " + d_sub_file + ".";
      if (stereo_settings().skip_low_res_disparity_comp)
        msg += " Perhaps one should disable --skip-low-res-disparity-comp.";
      vw_throw(ArgumentErr() << msg << "\n");
    }
  }
  ImageViewRef<PixelMask<Vector2i> > sub_disp_spread;
  if (stereo_settings().seed_mode == 2 ||  stereo_settings().seed_mode == 3){
    // D_sub_spread is mandatory for seed_mode 2 and 3.
    sub_disp_spread = DiskImageView<PixelMask<Vector2i> >(spread_file);
  }else if (stereo_settings().seed_mode == 1){
    // D_sub_spread is optional for seed_mode 1, we use it only if it is provided.
    if (fs::exists(spread_file)) {
      try {
        sub_disp_spread = DiskImageView<PixelMask<Vector2i> >(spread_file);
      }
      catch (...) {}
    }
  }

  stereo::CostFunctionType cost_mode = get_cost_mode_value();
  Vector2i kernel_size = stereo_settings().corr_kernel;
  BBox2i left_trans_crop_win = stereo_settings().trans_crop_win;
  int corr_timeout   = stereo_settings().corr_timeout;
  double seconds_per_op = 0.0;
  if (corr_timeout > 0)
    seconds_per_op = calc_seconds_per_op(cost_mode, left_disk_image, right_disk_image, kernel_size);

  // Set up the reference to the stereo disparity code
  // - Processing is limited to left_trans_crop_win for use with parallel_stereo.
  ImageViewRef<PixelMask<Vector2f> > fullres_disparity =
    crop(SeededCorrelatorView(left_disk_image, right_disk_image, Lmask, Rmask,
                              sub_disp, sub_disp_spread, kernel_size, 
                              cost_mode, corr_timeout, seconds_per_op), 
         left_trans_crop_win);

  // With SGM, we must do the entire image chunk as one tile. Otherwise,
  // if it gets done in smaller tiles, there will be artifacts at tile boundaries.
  vw::stereo::CorrelationAlgorithm stereo_alg
    = asp::stereo_alg_to_num(stereo_settings().stereo_algorithm);
  
  bool using_sgm = (stereo_alg > vw::stereo::VW_CORRELATION_BM &&
                    stereo_alg < vw::stereo::VW_CORRELATION_OTHER);
  if (using_sgm) {
    Vector2i image_size = bounding_box(fullres_disparity).size();
    int max_dim = std::max(image_size[0], image_size[1]);
    if (stereo_settings().corr_tile_size_ovr < max_dim)
      vw_throw(ArgumentErr()
               << "Error: SGM processing is not permitted with "
               << "a tile size smaller than the image!\n"
               << "Value of --corr-tile-size is " << stereo_settings().corr_tile_size_ovr
               << " but image size is " << image_size << ".\n" 
               << "Increase --corr-tile-size so the entire image fits in one tile, or "
               << "use parallel_stereo. Not that making --corr-tile-size "
               << "larger than 9000 or so may "
               << "cause GDAL to crash.\n\n");
  }
  
  switch(stereo_settings().pre_filter_mode){
  case 2:
    vw_out() << "\t--> Using LOG pre-processing filter with "
             << stereo_settings().slogW << " sigma blur.\n"; 
    break;
  case 1:
    vw_out() << "\t--> Using subtracted mean pre-processing filter with "
	     << stereo_settings().slogW << " sigma blur.\n";
    break;
  default:
    vw_out() << "\t--> Using NO pre-processing filter." << std::endl;
  }

  cartography::GeoReference left_georef;
  bool   has_left_georef = read_georeference(left_georef,  opt.out_prefix + "-L.tif");
  bool   has_nodata      = false;
  double nodata          = -32768.0;

  std::string d_file = opt.out_prefix + "-D.tif";
  vw_out() << "Writing: " << d_file << "\n";
  
  if (stereo_alg > vw::stereo::VW_CORRELATION_BM) {
    // SGM and external algorithms perform subpixel correlation in
    // this step, so write out floats.
    
    // Rasterize the image first as one block, then write it out using multiple blocks.
    // - If we don't do this, the output image file is not tiled and handles very slowly.
    // - This is possible because with SGM the image must be small enough to fit in memory.
    ImageView<PixelMask<Vector2f>> result = fullres_disparity;
    opt.raster_tile_size = Vector2i(ASPGlobalOptions::rfne_tile_size(),
                                    ASPGlobalOptions::rfne_tile_size());
    vw::cartography::block_write_gdal_image(d_file, result,
                                            has_left_georef, left_georef,
                                            has_nodata, nodata, opt,
                                            TerminalProgressCallback("asp", "\t--> Correlation :"));

  } else {
    // Otherwise cast back to integer results to save on storage space.
    vw::cartography::block_write_gdal_image(d_file, 
                                            pixel_cast<PixelMask<Vector2i>>(fullres_disparity),
                                            has_left_georef, left_georef,
                                            has_nodata, nodata, opt,
                                            TerminalProgressCallback("asp", "\t--> Correlation :"));
  }

} // End function stereo_correlation_2D

// A small function we will invoke repeatedly to save the disparity
void save_disparity(ASPGlobalOptions& opt,
                    ImageView<PixelMask<Vector2f>> unaligned_disp_2d,
                    std::string const& out_disp_file) {
  
  vw::cartography::GeoReference georef;
  bool   has_georef  = false;
  bool   has_nodata  = false;
  double nodata      = -32768.0;
  vw_out() << "Writing: " << out_disp_file << "\n";
  opt.raster_tile_size = Vector2i(ASPGlobalOptions::rfne_tile_size(),
                                  ASPGlobalOptions::rfne_tile_size());
  opt.gdal_options["TILED"] = "YES";
  vw::cartography::block_write_gdal_image(out_disp_file, unaligned_disp_2d,
                                          has_georef, georef,
                                          has_nodata, nodata, opt,
                                          TerminalProgressCallback
                                          ("asp", "\t--> Correlation :"));
}

// Write an empty disparity of given dimensions
void save_empty_disparity(ASPGlobalOptions& opt,
                          vw::BBox2i const& crop_win,
                          std::string const& out_disp_file) {

  vw::ImageView<PixelMask<Vector2f>> disp;
  disp.set_size(crop_win.width(), crop_win.height());
  
  for (int col = 0; col < disp.cols(); col++) {
    for (int row = 0; row < disp.rows(); row++) {
      disp(col, row) = PixelMask<Vector2f>();
      disp(col, row).invalidate();
    }
  }

  save_disparity(opt, disp, out_disp_file);
}
                    
/// Stereo correlation function using 1D correlation algorithms
/// (implemented in ASP and external ones). Local alignment will be
/// performed before those algorithms are invoked.
void stereo_correlation_1D(ASPGlobalOptions& opt) {

  // The low-res disparity computation, if desired, happens on the full images,
  // which is incompatible with local alignment and stereo for pairs of tiles.
  if (stereo_settings().compute_low_res_disparity_only) 
    return;
  
  // The dimensions of the tile and the final disparity
  BBox2i tile_crop_win = stereo_settings().trans_crop_win;

  // The left_trans_crop_win will be obtained by tile_crop_win by maybe growing it a bit
  BBox2i left_trans_crop_win, right_trans_crop_win;
  int max_tile_size = stereo_settings().corr_tile_size_ovr;
  Matrix<double> left_local_mat  = math::identity_matrix<3>();
  Matrix<double> right_local_mat = math::identity_matrix<3>();
  std::string left_aligned_file, right_aligned_file;
  int min_disp = -1, max_disp = -1;
  std::string out_disp_file = opt.out_prefix + "-D.tif";

  // Ensure the disparity is always recreated
  if (fs::exists(out_disp_file)) 
    fs::remove(out_disp_file);  

  std::string alg_name, user_opts;
  asp::parse_stereo_alg_name_and_opts(stereo_settings().stereo_algorithm,  
                                      alg_name, user_opts);
  vw_out() << "Using algorithm: " << alg_name << std::endl;

  // The msmw and msmw2 algorithms expects the input tif images to not
  // be tiled.  Accommodate it, then revert to the original when this
  // is no longer necessary.
  Vector2 orig_tile_size = opt.raster_tile_size;
  bool write_nodata = true;
  if (alg_name == "msmw" || alg_name == "msmw2") {
    opt.gdal_options["TILED"] = "NO";
    opt.raster_tile_size = vw::Vector2(-1, -1);
    write_nodata = false; // To avoid warnings from the tif reader in msmw
  }

  try {
    boost::shared_ptr<camera::CameraModel> left_camera_model, right_camera_model;
    opt.session->camera_models(left_camera_model, right_camera_model);
    cartography::Datum datum = opt.session->get_datum(left_camera_model.get(), false);
    local_alignment(// Inputs
                    opt, opt.session->name(),
                    max_tile_size, tile_crop_win,
                    write_nodata,
                    left_camera_model.get(),
                    right_camera_model.get(),
                    datum,
                    // Outputs
                    left_trans_crop_win, right_trans_crop_win,
                    left_local_mat, right_local_mat,
                    left_aligned_file, right_aligned_file,  
                    min_disp, max_disp);

  } catch(std::exception const& e){
    // If this tile fails, write an empty disparity
    vw_out() << e.what() << std::endl;
    save_empty_disparity(opt, tile_crop_win, out_disp_file);
    return;
  }
  
  vw_out() << "Min and max disparities: " << min_disp << ' ' << max_disp << ".\n";
  
  vw::ImageView<PixelMask<Vector2f>> unaligned_disp_2d;
  vw::stereo::CorrelationAlgorithm stereo_alg
    = asp::stereo_alg_to_num(stereo_settings().stereo_algorithm);
  
  if (stereo_alg < vw::stereo::VW_CORRELATION_OTHER) {

    // ASP algorithms

    // Mask the locally alignment images which were written with NaN nodata.
    float nan  = std::numeric_limits<float>::quiet_NaN();
    ImageView<PixelMask<PixelGray<float>>> left_image
      = vw::create_mask(DiskImageView<PixelGray<float>>(left_aligned_file), nan);
    ImageView<PixelMask<PixelGray<float>>> right_image
      = vw::create_mask(DiskImageView<PixelGray<float>>(right_aligned_file), nan);
    
    ImageView<vw::uint8> left_mask
      = channel_cast_rescale<vw::uint8>(select_channel(left_image, 1));
    ImageView<vw::uint8> right_mask
      = channel_cast_rescale<vw::uint8>(select_channel(right_image, 1));
    
    stereo::CostFunctionType cost_mode = get_cost_mode_value();
    Vector2i kernel_size = stereo_settings().corr_kernel;
    int corr_timeout = stereo_settings().corr_timeout;
    stereo_settings().seed_mode = 0; // no seed

    // The search range. Put here 2 for the upper limit in y as the
    // interval in y is [lower_limit, upper_limit).
    stereo_settings().search_range.min() = vw::Vector2i(min_disp, -1);
    stereo_settings().search_range.max() = vw::Vector2i(max_disp, 2);
    
    double seconds_per_op = 0.0;
    if (corr_timeout > 0)
      seconds_per_op = calc_seconds_per_op(cost_mode, left_image, right_image,
                                           kernel_size);

    // Start with no seed
    ImageView<PixelMask<Vector2f>> sub_disp;
    ImageView<PixelMask<Vector2i>> sub_disp_spread;

    // Find the disparity
    ImageView<PixelMask<Vector2f> > aligned_disp_2d =
      crop(SeededCorrelatorView(apply_mask(left_image, nan),    // left image
                                apply_mask(right_image, nan),   // right image
                                left_mask, right_mask,
                                sub_disp, sub_disp_spread, kernel_size, 
                                cost_mode, corr_timeout, seconds_per_op), 
           bounding_box(left_image));

#if 0
    // For debugging
    // Write the aligned 2D disparity to disk
    vw::cartography::GeoReference georef;
    bool   has_georef = false;
    bool   has_nodata = false;
    double nodata = std::numeric_limits<float>::quiet_NaN();
    std::string aligned_disp_file = opt.out_prefix + "-aligned-disparity_2D.tif";
    vw_out() << "Writing: " << aligned_disp_file << "\n";
    vw::cartography::block_write_gdal_image(aligned_disp_file, aligned_disp_2d,
                                            has_georef, georef,
                                            has_nodata, nan, opt,
                                            TerminalProgressCallback
                                            ("asp", "\t--> Disparity :"));
#endif
    
    // Undo the alignment
    asp::unalign_2d_disparity(// Inputs
                              aligned_disp_2d,
                              left_trans_crop_win, right_trans_crop_win,  
                              left_local_mat, right_local_mat,  
                              // Output
                              unaligned_disp_2d);
    
  } else if (stereo_alg == vw::stereo::VW_CORRELATION_OTHER) {

    // External algorithms using 1D disparity
    
    vw::ImageView<float> aligned_disp;

    // Set the default options for all algorithms
    
    std::string default_opts;
    if (alg_name == "mgm") {
      
      default_opts = std::string("MEDIAN=1 CENSUS_NCC_WIN=5 ")
        + "USE_TRUNCATED_LINEAR_POTENTIALS=1 TSGM=3 -s vfit -t census -O 8 "
        + "-r " + vw::num_to_str(min_disp) + " -R " + vw::num_to_str(max_disp);
      
    } else if (alg_name == "opencv_bm") {
      default_opts = std::string("-block_size 21 -texture_thresh 10 -prefilter_cap 31 ") +
        "-uniqueness_ratio 15 -speckle_size 100 -speckle_range 32 -disp12_diff 1";
      
    } else if (alg_name == "opencv_sgbm") {
      
      default_opts = std::string("-mode sgbm -block_size 3 -P1 8 -P2 32 -prefilter_cap 63 ") +
        "-uniqueness_ratio 10 -speckle_size 100 -speckle_range 32 -disp12_diff 1";
      
    } else if (alg_name == "msmw") {
      default_opts = std::string("-i 1 -n 4 -p 4 -W 5 -x 9 -y 9 -r 1 -d 1 -t -1 ")
        + "-s 0 -b 0 -o 0.25 -f 0 -P 32 "
        + "-m " + vw::num_to_str(min_disp) + " -M " + vw::num_to_str(max_disp);
      
    } else if (alg_name == "msmw2") {
      default_opts = std::string("-i 1 -n 4 -p 4 -W 5 -x 9 -y 9 -r 1 -d 1 -t -1 ")
        + "-s 0 -b 0 -o -0.25 -f 0 -P 32 -D 0 -O 25 -c 0 "
        + "-m " + vw::num_to_str(min_disp) + " -M " + vw::num_to_str(max_disp);
      
    } else if (alg_name == "libelas"){
      // For some reasons libelas fails with a tight search range
      int extra = 10 + std::max(0, min_disp);
      vw_out() << "For libelas, grow the search range on each end by " << extra << ".\n";
      default_opts = std::string("-support_threshold 0.85 -support_texture 10 ")
        + "-candidate_stepsize 5 -incon_window_size 5 "
        + "-incon_threshold 5 -incon_min_support 5 "
        + "-add_corners 0 -grid_size 20 "
        + "-beta 0.02 -gamma 3 -sigma 1 -sradius 2 "
        + "-match_texture 1 -lr_threshold 2 -speckle_sim_threshold 1 "
        + "-speckle_size 200 -ipol_gap_width 3 -filter_median 0 "
        + "-filter_adaptive_mean 1 -postprocess_only_left 0 "
        + "-disp_min " + vw::num_to_str(min_disp - extra) + " "
        + "-disp_max " + vw::num_to_str(max_disp + extra);
    }else {
      // No defaults for other algorithms
    }

    // Parse the algorithm options and environmental variables from the default
    // options and append the user options (the latter take precedence).
    std::string options, env_vars;
    std::map<std::string, std::string> option_map;
    std::map<std::string, std::string> env_vars_map;
    asp::extract_opts_and_env_vars(default_opts + " " + user_opts,  
                                   options, option_map,
                                   env_vars, env_vars_map);

    std::string aligned_disp_file = opt.out_prefix + "-aligned-disparity.tif";
    std::string mask_file = opt.out_prefix + "-disparity-mask.tif";

    // Ensure the disparity is always recreated
    if (fs::exists(aligned_disp_file)) 
      fs::remove(aligned_disp_file);  
    if (fs::exists(mask_file)) 
      fs::remove(mask_file);  
    
    if (alg_name == "opencv_bm") {
      // Call the OpenCV BM algorithm
      std::string mode = "bm";
      int dummy_p1 = -1, dummy_p2 = -1; // Only needed for SGBM
      call_opencv_bm_or_sgbm(left_aligned_file, right_aligned_file,
                             mode,
                             atoi(option_map["-block_size"].c_str()),  
                             min_disp, max_disp,  
                             atoi(option_map["-prefilter_cap"].c_str()),  
                             atoi(option_map["-uniqueness_ratio"].c_str()),  
                             atoi(option_map["-speckle_size"].c_str()),  
                             atoi(option_map["-speckle_range"].c_str()),  
                             atoi(option_map["-disp12_diff"].c_str()),  
                             atoi(option_map["-texture_thresh"].c_str()),
                             dummy_p1, dummy_p2,
                             opt, aligned_disp_file,  
                             // Output
                             aligned_disp);

    } else if (alg_name == "opencv_sgbm") {
      // Call the OpenCV SGBM algorithm
      int dummy_texture_thresh = -1; // only needed for BM
      call_opencv_bm_or_sgbm(left_aligned_file, right_aligned_file,
                             option_map["-mode"],
                             atoi(option_map["-block_size"].c_str()),  
                             min_disp, max_disp,  
                             atoi(option_map["-prefilter_cap"].c_str()),  
                             atoi(option_map["-uniqueness_ratio"].c_str()),  
                             atoi(option_map["-speckle_size"].c_str()),  
                             atoi(option_map["-speckle_range"].c_str()),  
                             atoi(option_map["-disp12_diff"].c_str()),  
                             dummy_texture_thresh,
                             atoi(option_map["-P1"].c_str()),  
                             atoi(option_map["-P2"].c_str()),  
                             opt, aligned_disp_file,  
                             // Output
                             aligned_disp);
    } else {

      // Read the list of plugins
      std::map<std::string, std::string> plugins, plugin_libs;
      asp::parse_plugins_list(plugins, plugin_libs);

      auto it1 = plugins.find(alg_name);
      auto it2 = plugin_libs.find(alg_name);
      if (it1 == plugins.end() || it2 == plugin_libs.end()) 
        vw_throw(ArgumentErr() << "Could not lookup plugin: " << alg_name << ".\n");
      
      std::string plugin_path = it1->second;
      std::string plugin_lib = it2->second;

      // Set up the environemnt
      bp::environment e = boost::this_process::environment();
      e["LD_LIBRARY_PATH"] = plugin_lib;   // For Linux
      e["DYLD_LIBRARY_PATH"] = plugin_lib; // For OSX
      vw_out() << "Path to libraries: " << plugin_lib << std::endl;
      for (auto it = env_vars_map.begin(); it != env_vars_map.end(); it++) {
        e[it->first] = it->second;
      }
      
      // Call an external program which will write the disparity to disk
      std::string cmd = plugin_path + " " + options + " " 
        + left_aligned_file + " " + right_aligned_file + " " + aligned_disp_file;
      
      if (alg_name == "msmw" || alg_name == "msmw2") {
        // Need to provide the output mask
        cmd += " " + mask_file;
      }

      int timeout = stereo_settings().corr_timeout;

      if (env_vars != "") 
        vw_out() << "Using environmental variables: " << env_vars << std::endl;

      vw_out() << cmd << std::endl;

      // Use boost::process to run the given process with timeout.
      bp::child c(cmd, e);
      std::error_code ec;
      if (!c.wait_for(std::chrono::seconds(timeout), ec)) {
        vw_out() << "\n" << "Timeout reached. Process terminated after "
                 << timeout << " seconds. See the --corr-timeout option.\n";
        c.terminate(ec);
      }      
        
      // Read the disparity from disk. This may fail, for example, the
      // disparity may time out or it may not have good data. In that
      // case just make an empty disparity, as we don't want
      // the processing of the full image to fail because of a tile.
      try {
        aligned_disp = DiskImageView<float>(aligned_disp_file);
      } catch(std::exception const& e){
        // If this tile fails, write an empty disparity
        vw_out() << e.what() << std::endl;
        save_empty_disparity(opt, tile_crop_win, out_disp_file);
        return;
      }
      
      if (alg_name == "msmw" || alg_name == "msmw2") {
        // Apply the mask, which for this algorithm is stored separately.
        // For that need to read things in memory.
        ImageView<float> local_disp(aligned_disp.cols(), aligned_disp.rows());
        DiskImageView<vw::uint8> mask(mask_file);

        if (local_disp.cols() != mask.cols() || local_disp.rows() != mask.rows()) 
          vw_throw(ArgumentErr() << "Expecting that the following images would "
                   << "have the same dimensions: "
                   << aligned_disp_file << ' ' << mask_file << ".\n");
          
        float nan = std::numeric_limits<float>::quiet_NaN();
        for (int col = 0; col < local_disp.cols(); col++) {
          for (int row = 0; row < local_disp.rows(); row++) {
            if (mask(col, row) != 0) 
              local_disp(col, row) = aligned_disp(col, row);
            else
              local_disp(col, row) = nan;
          }
        }
        
        // Assign the image we just made to the handle
        aligned_disp = local_disp;
      }
    }

    try {
      // Sanity check. Temporarily load the left image.
      DiskImageView<float> left_image(left_aligned_file);
      if (aligned_disp.cols() != left_image.cols() || 
          aligned_disp.rows() != left_image.rows() ) 
        vw_throw(ArgumentErr() << "Expecting that the 1D disparity " << aligned_disp_file
                 << " would have the same dimensions as the left image " << left_aligned_file
                 << ".\n");
    } catch(std::exception const& e){
      // If this tile fails, write an empty disparity
      vw_out() << e.what() << std::endl;
      save_empty_disparity(opt, tile_crop_win, out_disp_file);
      return;
    }

    if (0) {
      // This needs more testing.
      // TODO(oalexan1): Make this into a function. Filter the disparity.
      // Wipe disparities which map to an invalid pixel
      float nan  = std::numeric_limits<float>::quiet_NaN();
      ImageView<PixelMask<PixelGray<float>>> left_masked_image
        = vw::create_mask(DiskImageView<PixelGray<float>>(left_aligned_file), nan);
      ImageView<PixelMask<PixelGray<float>>> right_masked_image
        = vw::create_mask(DiskImageView<PixelGray<float>>(right_aligned_file), nan);
      
      // invalid value for a PixelMask
      PixelMask<PixelGray<float>> nodata_mask = PixelMask<PixelGray<float>>(); 
      ImageViewRef<PixelMask<PixelGray<float>>> interp_right_masked_image
      = interpolate(right_masked_image, BilinearInterpolation(),
                    ValueEdgeExtension<PixelMask<PixelGray<float>>>(nodata_mask));
      
      for (int col = 0; col < left_masked_image.cols(); col++) {
        for (int row = 0; row < left_masked_image.rows(); row++) {

          if (std::isnan(aligned_disp(col, row))) 
            continue;  // already nan

          // If the left pixel is not valid, the disparity cannot be valid
          if (!is_valid(left_masked_image(col, row))) {
            aligned_disp(col, row) = nan;
            continue;
          }

          // If the right pixel is not valid, the disparity cannot be valid
          Vector2 right_pix(col + aligned_disp(col, row), row);
          if (!is_valid(interp_right_masked_image(right_pix.x(), right_pix.y()))) 
            aligned_disp(col, row) = nan;
        }
      }
    }
    
    // Undo the alignment
    asp::unalign_1d_disparity(// Inputs
                              aligned_disp,
                              left_trans_crop_win, right_trans_crop_win,  
                              left_local_mat, right_local_mat,  
                              // Output
                              unaligned_disp_2d);
  }

  // Undo the logic needed for msmw
  if (alg_name == "msmw" || alg_name == "msmw2") {
    opt.gdal_options["TILED"] = "YES";
    opt.raster_tile_size = orig_tile_size;
  }

  // Adjust for the fact that tile_crop_win may not be the same as left_trans_crop_win.
  vw::ImageView<PixelMask<Vector2f>> cropped_disp(tile_crop_win.width(), tile_crop_win.height());
  for (int col = 0; col < tile_crop_win.width(); col++) {
    for (int row = 0; row < tile_crop_win.height(); row++) {
      Vector2 pix = Vector2(col, row) + tile_crop_win.min();
      if (left_trans_crop_win.contains(pix)) {
        cropped_disp(col, row)
          = unaligned_disp_2d(pix.x() - left_trans_crop_win.min().x(),
                              pix.y() - left_trans_crop_win.min().y());
      } else {
        cropped_disp(col, row) = PixelMask<Vector2f>();
        cropped_disp(col, row).invalidate();
      }
      
    }
  }
  
  save_disparity(opt, cropped_disp, out_disp_file);

} // End function stereo_correlation_1D

int main(int argc, char* argv[]) {

  try {
    xercesc::XMLPlatformUtils::Initialize();

    stereo_register_sessions();

    bool verbose = false;
    std::vector<ASPGlobalOptions> opt_vec;
    std::string output_prefix;
    asp::parse_multiview(argc, argv, CorrelationDescription(),
                         verbose, output_prefix, opt_vec);
    ASPGlobalOptions opt = opt_vec[0];

    // Leave the number of parallel block threads equal to the default unless we
    //  are using SGM in which case only one block at a time should be processed.
    // - Processing multiple blocks is possible, but it is better to use a larger blocks
    //   with more threads applied to the single block.
    // - Thread handling is still a little confusing because opt.num_threads is ONLY used
    //   to control the number of parallel image blocks written at a time.  Everything else
    //   reads directly from vw_settings().default_num_threads()
    vw::stereo::CorrelationAlgorithm stereo_alg
      = asp::stereo_alg_to_num(stereo_settings().stereo_algorithm);
    bool using_sgm = (stereo_alg > vw::stereo::VW_CORRELATION_BM &&
                      stereo_alg < vw::stereo::VW_CORRELATION_OTHER);
    opt.num_threads = vw_settings().default_num_threads();
    if (using_sgm)
      opt.num_threads = 1;

    // Integer correlator requires large tiles
    //---------------------------------------------------------
    int ts = stereo_settings().corr_tile_size_ovr;
    
    // GDAL block write sizes must be a multiple to 16 so if the input value is
    //  not a multiple of 16 increase it until it is.
    const int TILE_MULTIPLE = 16;
    if (ts % TILE_MULTIPLE != 0)
      ts = ((ts / TILE_MULTIPLE) + 1) * TILE_MULTIPLE;
      
    opt.raster_tile_size = Vector2i(ts, ts);

    vw_out() << "\n[ " << current_posix_time_string() << " ] : Stage 1 --> CORRELATION\n";

    if (stereo_settings().alignment_method == "local_epipolar") {
      // Need to have the low-res 2D disparity to later guide the
      // per-tile correlation. Use here the ASP MGM algorithm as the
      // most reliable one.
      if (stereo_settings().compute_low_res_disparity_only) {
        stereo_settings().stereo_algorithm = "asp_mgm";
        stereo_correlation_2D(opt);
        return 0;
      }
      // This will be invoked per-tile.
      stereo_correlation_1D(opt);
    } else {
      // Do 2D correlation. The first time this is invoked it will
      // compute the low-res disparity unless told not to.
      stereo_correlation_2D(opt);
    }

    vw_out() << "\n[ " << current_posix_time_string() << " ] : CORRELATION FINISHED\n";
    
    xercesc::XMLPlatformUtils::Terminate();
  } ASP_STANDARD_CATCHES;

  return 0;
}
