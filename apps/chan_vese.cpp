#include <iostream> // std::cout, std::cerr
#include <cstdlib> // EXIT_SUCCESS, EXIT_FAILURE
#include <vector> // std::vector<>
#include <algorithm> // std::min(), std::max()
#include <cmath> // std::pow(), std::sqrt(), std::sin(), std::atan()
#include <exception> // std::exception
#include <string> // std::string, std::to_string()
#include <functional> // std::function<>, std::bind(), std::placeholders::_1
#include <limits> // std::numeric_limits<>
#include <map> // std::map<>
#include <fstream>
#include <iterator>


#include <boost/algorithm/string/predicate.hpp> // boost::iequals()
#include <boost/algorithm/string/join.hpp> // boost::algorithm::join()

#include <boost/program_options/options_description.hpp> // boost::program_options::options_description, boost::program_options::value<>
#include <boost/program_options/variables_map.hpp> // boost::program_options::variables_map,
												   // boost::program_options::store(),
												   // boost::program_options::notify()
#include <boost/program_options/parsers.hpp> // boost::program_options::cmd_line::parser
#include <boost/filesystem/operations.hpp>   // boost::filesystem::exists()
#include <boost/filesystem/convenience.hpp>  // boost::filesystem::change_extension()

#include "dicom_reader.hpp"
#include "contour_extraction.hpp"

/// Adds suffix to the file name
std::string add_suffix(const std::string & path, const std::string & suffix, const std::string & delim = "_")
{
	namespace fs = boost::filesystem;
	const fs::path p(path);
	const fs::path nw_p = p.parent_path() / fs::path(p.stem().string() + delim + suffix + p.extension().string());
	return nw_p.string();
}


/// Displays error message surrounded by newlines and exits.
void msg_exit(const std::string & msg)
{
	std::cerr << "\n" << msg << "\n\n";
	std::exit(EXIT_FAILURE);
}

cv::Point2d mean_shift(const cv::Mat1d& img, const cv::Point2d init, const double thresh = 0.75, const size_t max_iter = 5) {
	cv::Mat1d gb_img, gb_img_draw;

	const size_t max_size = std::max(img.cols, img.rows);
	const size_t gb_side_size = 2 * size_t(0.015 * max_size) + 1;
	//cv::GaussianBlur(img, gb_img, cv::Size(gb_side_size, gb_side_size), 0, 0);
	gb_img = img.clone();
	cv::Point2d cur_pose(init), prev_pose(0, 0);

	const size_t sz = (10. / 256.) * max_size;

	cv::Mat1d mask = cv::Mat1d::zeros(2*sz+1, 2*sz+1);
	cv::circle(mask, cv::Point(sz, sz), sz, cv::Scalar::all(1.), -1);

	int iter{};
	while (cv::norm(cur_pose - prev_pose) > thresh) {
		prev_pose = cur_pose;
		cv::Mat1d roi(gb_img(cv::Rect(cur_pose.x - sz + 0.5, cur_pose.y - sz + 0.5, 2 * sz + 1, 2 * sz + 1)));
		roi = roi.mul(mask);
		cv::Moments moments = cv::moments(roi, false);
		cv::Point2d diff(moments.m10 / moments.m00, moments.m01 / moments.m00);
		cur_pose = cur_pose + (diff - cv::Point2d(sz, sz));
		if (iter++ > max_iter) break;
	}
	return cur_pose;
};

cv::Mat1b rectify_lv_segment(const cv::Mat1d& img, const cv::Point& seed, const std::vector<std::vector<cv::Point>>& contours, const int target_idx) 
{
	cv::Mat1b watershed_contours(img.size(), 0);
	cv::drawContours(watershed_contours, contours, target_idx, cv::Scalar(255, 255, 255), -1);

	cv::Mat opening, sure_bg, sure_fg;
	cv::Mat kernel = cv::getStructuringElement(cv::MORPH_ELLIPSE, { 3,3 });
	cv::morphologyEx(watershed_contours, opening, cv::MORPH_OPEN, kernel);
	cv::dilate(opening, sure_bg, kernel, { -1,-1 }, 3);

	cv::Mat markers_dt;
	cv::distanceTransform(watershed_contours, markers_dt, cv::DistanceTypes::DIST_L2, cv::DistanceTransformMasks::DIST_MASK_3);
	double max;
	cv::minMaxLoc(markers_dt, nullptr, &max);
	cv::Mat markers;
	cv::threshold(markers_dt, markers, 0.6 * max, 255, cv::THRESH_BINARY);
	sure_fg = markers != 0;

	cv::Mat unknown = sure_bg - sure_fg;
	markers.convertTo(markers, CV_8UC1);
	cv::connectedComponents(markers, markers);
	markers += 1;
	markers.setTo(0, unknown);

	cv::Mat ws_markers = markers.clone();
	cv::Mat3b watershed_contours_3b;
	cv::merge(std::vector<cv::Mat1b>{watershed_contours, watershed_contours, watershed_contours}, watershed_contours_3b);
	cv::watershed(watershed_contours_3b, ws_markers);

	cv::Mat1b lv_mask = ws_markers == ws_markers.at<int>(seed);
	cv::dilate(lv_mask, lv_mask, cv::getStructuringElement(cv::MORPH_RECT, { 3,3 }));
	return lv_mask;
};

int main(int argc, char ** argv)
{
	PeronaMalikArgs pm_args;
	struct ChanVeseArgs cv_args;

	std::string input_patient;

	bool object_selection = false;
	bool segment = false;
	bool show_windows = false;

	//-- Parse command line arguments
	//   Negative values in multitoken are not an issue, b/c it doesn't make much sense
	//   to use negative values for lambda1 and lambda2
	try {
		namespace po = boost::program_options;
		po::options_description desc("Allowed options", 120);
		desc.add_options()
			("help,h", "this message")
			("input,i", po::value<std::string>(&input_patient), "patient directory")
			("mu", po::value<double>(&cv_args.mu)->default_value(0.5), "length penalty parameter (must be positive or zero)")
			("nu", po::value<double>(&cv_args.nu)->default_value(0), "area penalty parameter")
			("dt", po::value<double>(&cv_args.dt)->default_value(1), "timestep")
			("lambda2", po::value<double>(&cv_args.lambda2)->default_value(1.), "penalty of variance outside the contour (default: 1's)")
			("lambda1", po::value<double>(&cv_args.lambda1)->default_value(1.), "penalty of variance inside the contour (default: 1's)")
			("epsilon,e", po::value<double>(&cv_args.eps)->default_value(1), "smoothing parameter in Heaviside/delta")
			("tolerance,t", po::value<double>(&cv_args.tol)->default_value(0.001), "tolerance in stopping condition")
			("max-steps,N", po::value<int>(&cv_args.max_steps)->default_value(1000), "maximum nof iterations (negative means unlimited)")
			("edge-coef,K", po::value<double>(&pm_args.K)->default_value(10), "coefficient for enhancing edge detection in Perona-Malik")
			("laplacian-coef,L", po::value<double>(&pm_args.L)->default_value(0.25), "coefficient in the gradient FD scheme of Perona-Malik (must be [0, 1/4])")
			("segment-time,T", po::value<double>(&pm_args.T)->default_value(20), "number of smoothing steps in Perona-Malik")
			("segment,S", po::bool_switch(&segment), "segment the image with Perona-Malik beforehand")
			("select,s", po::bool_switch(&object_selection), "separate the region encolosed by the contour (adds suffix '_selection')")
			("show", po::bool_switch(&show_windows), "");
		po::variables_map vm;
		po::store(po::command_line_parser(argc, argv).options(desc).run(), vm);
		po::notify(vm);

		if (vm.count("help")) {
			std::cout << desc << "\n";
			return EXIT_SUCCESS;
		}
		if (!vm.count("input")) msg_exit("Error: you have to specify input file name!");
		if (vm.count("input") && !boost::filesystem::exists(input_patient)) msg_exit("Error: file \"" + input_patient + "\" does not exists!");
		if (vm.count("dt") && cv_args.dt <= 0) msg_exit("Cannot have negative or zero timestep: " + std::to_string(cv_args.dt) + ".");
		if (vm.count("mu") && cv_args.mu < 0) msg_exit("Length penalty parameter cannot be negative: " + std::to_string(cv_args.mu) + ".");
		if (vm.count("lambda1") && cv_args.lambda1 < 0) msg_exit("Any value of lambda1 cannot be negative.");
		if (vm.count("lambda2") && cv_args.lambda2 < 0) msg_exit("Any value of lambda2 cannot be negative.");
		if (vm.count("eps") && cv_args.eps < 0) msg_exit("Cannot have negative smoothing parameter: " + std::to_string(cv_args.eps) + ".");
		if (vm.count("tol") && cv_args.tol < 0) msg_exit("Cannot have negative tolerance: " + std::to_string(cv_args.tol) + ".");
		if (vm.count("laplacian-coef") && (pm_args.L > 0.25 || pm_args.L < 0)) msg_exit("The Laplacian coefficient in Perona-Malik segmentation must be between 0 and 0.25.");
		if (vm.count("segment-time") && (pm_args.T < pm_args.L)) msg_exit("The segmentation duration must exceed the value of Laplacian coefficient, " + std::to_string(pm_args.L) + ".");
	}
	catch (std::exception & e) {
		msg_exit("error: " + std::string(e.what()));
	}

	PatientData patient_data(input_patient);

	auto& sax = patient_data.sax_seqs[8];
	cv::Vec3d point_3d = slices_intersection(patient_data.ch2_seq.slices[0], patient_data.ch4_seq.slices[0], sax.slices[0]);
	cv::Point2d point = sax.point_to_image(point_3d);

	cv::Mat1d img = sax.slices[11].image;
	std::string input_filename = sax.slices[0].filename;


	//-- Determine the constants and define functionals
	cv_args.max_steps = cv_args.max_steps < 0 ? std::numeric_limits<int>::max() : cv_args.max_steps;
	double max_size(std::max(img.cols, img.rows));
	double pixel_scale = 1.0;
	if (max_size > 256) {
		pixel_scale = 256. / max_size;
		cv::resize(img, img, cv::Size(), pixel_scale, pixel_scale, cv::INTER_CUBIC);
		max_size = std::max(img.cols, img.rows);
	}

#if 0
	cv::Size ms_window_sz(8, 8);
	cv::Rect ms_window(cv::Point(point.x * pixel_scale - ms_window_sz.width / 2, point.y * pixel_scale - ms_window_sz.height / 2), ms_window_sz);
	cv::meanShift(img, ms_window, cv::TermCriteria(CV_TERMCRIT_ITER | CV_TERMCRIT_EPS, 20, 1));
	const cv::Point seed = (ms_window.br() + ms_window.tl()) / 2;
#else
	const cv::Point seed = mean_shift(img, { point.x * pixel_scale, point.y * pixel_scale });
#endif

	//-- Construct the level set
	cv::Mat1d cv_init = cv::Mat1d::zeros(img.size());
	cv::circle(cv_init, seed, 2, cv::Scalar::all(1), 1);


	//-- Smooth the image with Perona-Malik
	const cv::Mat1d abs_img = cv::abs(img - cv::mean(img(cv::Rect(seed - cv::Point(2, 2), cv::Size(5, 5))))[0]);
	const cv::Mat smoothed_img = perona_malik(img, pm_args);

	// Actual Chen-Vese segmentation
	cv::Mat1d chan_vese_image = segmentation_chan_vese(smoothed_img, cv_init, cv_args);

	//-- Select the region enclosed by the contour and save it to the disk
	cv::Mat chan_vese_segmentation = separate(img, chan_vese_image);




	std::vector<std::vector<cv::Point> > contours;
	cv::Mat1b separated8uc;
	chan_vese_segmentation.convertTo(separated8uc, CV_8UC1, 255);
	separated8uc = separated8uc != 0;
	findContours(separated8uc, contours, {}, CV_RETR_LIST, CV_CHAIN_APPROX_NONE);
	size_t target_idx = std::distance(contours.begin(), std::find_if(contours.begin(), contours.end(), [&](std::vector<cv::Point>& contour) { return 0 < cv::pointPolygonTest(contour, seed, false); }));

	cv::Mat1b final_mask = rectify_lv_segment(img, seed, contours, target_idx);

	findContours(final_mask, contours, {}, CV_RETR_LIST, CV_CHAIN_APPROX_NONE);
	target_idx = 0;

	cv::Mat3d imgc;
	cv::merge(std::vector<cv::Mat1d>{ img, img, img }, imgc);
	cv::circle(imgc, seed, 2, cv::Scalar(0., 0., 255.), -1);
	cv::circle(imgc, cv::Point(point.x * pixel_scale, point.y * pixel_scale), 2, cv::Scalar(255., 0., 0.), -1);
	
	if (false && contours[target_idx].size() > 20) {
		cv::RotatedRect box = ::fitEllipse(contours[target_idx], seed, img.size());
		cv::ellipse(imgc, box, cv::Scalar(0, 0, 255), 1, cv::LINE_AA);
	}

	cv::drawContours(imgc, contours, target_idx, cv::Scalar(0, 255, 0));

	{
		double min, max;
		cv::minMaxLoc(smoothed_img, &min, &max);
		cv::imwrite(add_suffix(input_filename, "pm") + ".png", smoothed_img);
		cv::imwrite(add_suffix(input_filename, "selection") + ".png", chan_vese_segmentation);
		cv::imwrite(add_suffix(input_filename, "contour") + ".png", imgc);

		std::ofstream output(add_suffix(input_filename, "contour") + ".csv");
		std::transform(contours[target_idx].begin(), contours[target_idx].end(), std::ostream_iterator<std::string>(output, ","),
			[&](const cv::Point& p) {
			return std::to_string(p.x / pixel_scale) + "," + std::to_string(p.y / pixel_scale);
		});

	}

	if (show_windows) {
		double min, max;
		cv::minMaxLoc(img, &min, &max);
		max *= 0.5;
		chan_vese_segmentation = (chan_vese_segmentation - min) / (max - min);
		imgc = (imgc - min) / (max - min);
		cv::minMaxLoc(smoothed_img, &min, &max);
		cv::imshow("input", imgc);
		cv::imshow("smoothed", (smoothed_img - min) / (max - min));
		cv::imshow("separated", chan_vese_segmentation);
	}

	if (show_windows) cv::waitKey();
	return EXIT_SUCCESS;
}
