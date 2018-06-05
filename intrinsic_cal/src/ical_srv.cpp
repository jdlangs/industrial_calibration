#include <ros/ros.h>

#include <string>
#include <fstream>

// ros includes
#include <ros/ros.h>
#include <ros/package.h>
#include <ros/console.h>
#include <std_srvs/Trigger.h>


// ros actionlib includes
#include <actionlib/server/simple_action_server.h>
#include <actionlib/client/simple_action_client.h>

// intrinsic_cal includes
#include <intrinsic_cal/ical_srv_solve.h>

// industrial_extrinsic_cal includes
#include <industrial_extrinsic_cal/basic_types.h>
#include <industrial_extrinsic_cal/camera_observer.hpp>
#include <industrial_extrinsic_cal/camera_definition.h>
#include <industrial_extrinsic_cal/observation_scene.h>
#include <industrial_extrinsic_cal/observation_data_point.h>
#include <industrial_extrinsic_cal/ceres_blocks.h>
#include <industrial_extrinsic_cal/ros_camera_observer.h>
#include <industrial_extrinsic_cal/ceres_costs_utils.hpp>
#include <industrial_extrinsic_cal/ceres_costs_utils.h>
#include <industrial_extrinsic_cal/circle_cost_utils.hpp>
#include <industrial_extrinsic_cal/calibration_job_definition.h>
#include <industrial_extrinsic_cal/calibrationAction.h>
#include <industrial_extrinsic_cal/calibrate.h>
#include <industrial_extrinsic_cal/covariance.h>
#include <industrial_extrinsic_cal/ros_target_display.hpp>
#include <industrial_extrinsic_cal/camera_yaml_parser.h>
#include <industrial_extrinsic_cal/targets_yaml_parser.h>


// boost includes
#include <boost/foreach.hpp>
#include <boost/shared_ptr.hpp>

using boost::shared_ptr;
using ceres::Solver;
using ceres::CostFunction;
using industrial_extrinsic_cal::Camera;
using industrial_extrinsic_cal::Target;
using industrial_extrinsic_cal::CeresBlocks;
using industrial_extrinsic_cal::Roi;
using industrial_extrinsic_cal::CameraObservations;
using industrial_extrinsic_cal::Point3d;



class icalServiceNode
{
public:
  icalServiceNode(const  ros::NodeHandle& nh)
    :nh_(nh), P_(NULL), problem_initialized_(false), total_observations_(0)
  {

    std::string nn = ros::this_node::getName();
    ros::NodeHandle priv_nh("~");

    // load cameras and targets
    priv_nh.getParam("yaml_file_path", yaml_file_path_);
    priv_nh.getParam("camera_file", camera_file_);
    priv_nh.getParam("target_file", target_file_);
    ROS_INFO("yaml_file_path: %s", yaml_file_path_.c_str());
    ROS_INFO("camera_file: %s", camera_file_.c_str());
    ROS_INFO("target_file: %s", target_file_.c_str());
    if(!load_camera()){
      ROS_ERROR("can't load the camera from %s", (yaml_file_path_+camera_file_).c_str());
    }
    if(!load_target()){
      ROS_ERROR("can't load the target from %s", (yaml_file_path_+target_file_).c_str());
    }

    // load cameras, targets and intiialize ceres blocks
    load_camera();
    load_target();
    init_blocks();

    // advertise services
    start_server_       = nh_.advertiseService( "IcalSrvStart", &icalServiceNode::startCallBack, this);
    observation_server_ = nh_.advertiseService( "IcalSrvObs", &icalServiceNode::observationCallBack, this);
    run_server_         = nh_.advertiseService( "IcalSrvRun", &icalServiceNode::runCallBack, this);
    save_server_        = nh_.advertiseService( "IcalSrvSave", &icalServiceNode::saveCallBack, this);
  };// end of constructor

  void init_blocks()
  {
    
    // add one block for each static camera
    // add one block with scene=0 for moving cameras (new set of extrinsic params for each scene_id)
    // add one block for each static target
    // add one block with each scene=0 for moving targets (new set of extrinsic params for each scene_id
    for (int i = 0; i < (int)all_cameras_.size(); i++)
      {
	if (all_cameras_[i]->is_moving_)
	  {
	    int scene_id = 0;
	    ceres_blocks_.addMovingCamera(all_cameras_[i], scene_id);
	  }
	else
	  {
	    ceres_blocks_.addStaticCamera(all_cameras_[i]);
	  }
      }


    for (int i = 0; i < (int)all_targets_.size(); i++)
      {
	if (all_targets_[i]->pub_rviz_vis_){ // use rviz visualization marker to display the target, currently must be modified circle grid
	  displayRvizTarget(all_targets_[i]);
	}
	if (all_targets_[i]->is_moving_)
	  {
	    int scene_id = 0;
	    ceres_blocks_.addMovingTarget(all_targets_[i], scene_id);
	  }
	else
	  {
	    ceres_blocks_.addStaticTarget(all_targets_[i]);
	  }
      }  // end for every target found


    // if loaded camera is the right side of a stereo pair, it contains a pointer to the left one.
    for (int i = 0; i < (int)all_cameras_.size(); i++)
      {
	if(all_cameras_[i]->is_right_stereo_camera_)
	  {
	    all_cameras_[i]->left_stereo_camera_ = ceres_blocks_.getCameraByName(all_cameras_[i]->left_stereo_camera_name_);
	  }
      }
  }; // end init_blocks()

  bool load_camera() 
  {
    bool rtn = true;
    if (!parseCameras(camera_file_, all_cameras_))
      {
	ROS_ERROR("failed to parse cameras from %s", camera_file_.c_str());
	rtn = false;
      }
    return rtn;
  };// end of load_camera()

  bool load_target()
  {
    bool rtn = true;

    if (!parseTargets(target_file_, all_targets_))
      {
	ROS_ERROR("failed to parse targets from %s", target_file_.c_str());
	rtn = false;
      }
    return rtn;
  }; // end load_target()

  // callback functions
  bool startCallBack( std_srvs::TriggerRequest &req, std_srvs::TriggerResponse &res)
  {
    if(problem_initialized_)	delete(P_);
    P_ = new ceres::Problem;
    problem_initialized_ = true;
    total_observations_  = 0;
    ceres_blocks_.clearCamerasTargets(); 
  }

  bool observationCallBack( std_srvs::TriggerRequest &req, std_srvs::TriggerResponse &res)
  {
    if(!problem_initialized_){
      ROS_ERROR("must calll start service");
      return(false);
    }


    for(int i=0; i<all_cameras_.size(); i++){
      // set the roi to the whole image
      Roi roi;
      roi.x_min = 0;
      roi.y_min = 0;
      roi.x_max = all_cameras_[i]->camera_parameters_.width;
      roi.y_max = all_cameras_[i]->camera_parameters_.height;

      // get observations
      all_cameras_[i]->camera_observer_->clearTargets();
      all_cameras_[i]->camera_observer_->clearObservations();
      industrial_extrinsic_cal::Cost_function cost_type = industrial_extrinsic_cal::cost_functions::CameraReprjErrorWithDistortion;
      int total_pts=0;
      for(int j=0;j<all_targets_.size();j++){ // add all targets to the camera
	all_cameras_[i]->camera_observer_->addTarget(all_targets_[j], roi, cost_type);
	total_pts += all_targets_[j]->num_points_;
      }
      all_cameras_[i]->camera_observer_->triggerCamera();
      while (!all_cameras_[i]->camera_observer_->observationsDone());
      CameraObservations camera_observations;
      all_cameras_[i]->camera_observer_->getObservations(camera_observations);
      int num_observations = (int)camera_observations.size();
      ROS_INFO("Found %d observations", (int)camera_observations.size());
	
      // add observations to problem
      num_observations = (int)camera_observations.size();
      if (num_observations != total_pts)
	{
	  ROS_ERROR("Target Locator could not find all targets found %d out of %d", num_observations, total_pts);
	}
      else
	{
	  // add a new cost to the problem for each observation
	  CostFunction* cost_function[num_observations];  
	  total_observations_ += num_observations;
	  for (int i = 0; i < num_observations; i++)
	    {
	      shared_ptr<Target> target = camera_observations[i].target;
	      double image_x = camera_observations[i].image_loc_x;
	      double image_y = camera_observations[i].image_loc_y;
	      Point3d point  = target->pts_[i];
	      cost_function[i] = industrial_extrinsic_cal::CircleCameraReprjErrorWithDistortionPK::Create(image_x, image_y, target->circle_grid_parameters_.circle_diameter, point);
	      P_->AddResidualBlock(cost_function[i], NULL, all_cameras_[i]->camera_parameters_.pb_intrinsics, target->pose_.pb_pose);
	    }  // for each observation at this camera_location
	}    // for each camera_location
    }
  }; // end observation service

  bool runCallBack( intrinsic_cal::ical_srv_solveRequest &req, intrinsic_cal::ical_srv_solveResponse &res)
  {

    // check for obvious errors
    if(!problem_initialized_){
      ROS_ERROR("must calll start service");
      return(false);
    }
    if(total_observations_ == 0){
      ROS_ERROR("must call observations service at least once");
      return(false);
    }

    Solver::Options options;
    Solver::Summary summary;
    options.linear_solver_type = ceres::DENSE_SCHUR;
    options.minimizer_progress_to_stdout = true;
    options.max_num_iterations = 2000;
    ceres::Solve(options, P_, &summary);
    if (summary.termination_type != ceres::NO_CONVERGENCE)
      {
	double initial_cost = summary.initial_cost / total_observations_;
	double final_cost = summary.final_cost / total_observations_;
	ROS_INFO("Problem solved, initial cost = %lf, final cost = %lf", initial_cost, final_cost);
	for(int j=0;j<all_targets_.size();j++){
	  all_targets_[j]->pose_.show("target_pose");
	}
	for(int i=0; i<all_cameras_.size(); i++){
	  ROS_INFO("camera_matrix data: [ %lf, 0.0, %lf, 0.0, %lf, %lf, 0.0, 0.0, 1.0]",
		   all_cameras_[i]->camera_parameters_.focal_length_x, all_cameras_[i]->camera_parameters_.center_x,
		   all_cameras_[i]->camera_parameters_.focal_length_y, all_cameras_[i]->camera_parameters_.center_y);
	  ROS_INFO("distortion data: [ %lf,  %lf,  %lf,  %lf,  %lf]", all_cameras_[i]->camera_parameters_.distortion_k1,
		   all_cameras_[i]->camera_parameters_.distortion_k2, all_cameras_[i]->camera_parameters_.distortion_p1,
		   all_cameras_[i]->camera_parameters_.distortion_p2, all_cameras_[i]->camera_parameters_.distortion_k3);
	  ROS_INFO("projection_matrix data: [ %lf, 0.0, %lf, 0.0, 0.0, %lf, %lf, 0.0, 0.0, 0.0, 1.0, 0.0]",
		   all_cameras_[i]->camera_parameters_.focal_length_x, all_cameras_[i]->camera_parameters_.center_x,
		   all_cameras_[i]->camera_parameters_.focal_length_y, all_cameras_[i]->camera_parameters_.center_y);
	}
	if (final_cost <= req.allowable_cost_per_observation)
	  {
	    ROS_INFO("calibration was successful");
	  }
	else
	  {
	    res.final_cost_per_observation = final_cost;
	    ROS_ERROR("allowable cost exceeded %f > %f", final_cost, req.allowable_cost_per_observation);
	    return (false);
	  }
	return(true);
      }
    ROS_ERROR("NO CONVERGENCE");
    return(false);
  }; // end runCallBack()

  bool saveCallBack( std_srvs::TriggerRequest &req, std_srvs::TriggerResponse &res)
  {
    for(int i=0; i<all_cameras_.size(); i++){
      all_cameras_[i]->camera_observer_->pushCameraInfo(
							all_cameras_[i]->camera_parameters_.focal_length_x, all_cameras_[i]->camera_parameters_.focal_length_y,
							all_cameras_[i]->camera_parameters_.center_x, all_cameras_[i]->camera_parameters_.center_y,
							all_cameras_[i]->camera_parameters_.distortion_k1, all_cameras_[i]->camera_parameters_.distortion_k2,
							all_cameras_[i]->camera_parameters_.distortion_k3, all_cameras_[i]->camera_parameters_.distortion_p1,
							all_cameras_[i]->camera_parameters_.distortion_p2);
    }
  }// end saveCallback()

private:
  ros::NodeHandle nh_;
  std::vector<shared_ptr<Camera> > all_cameras_;
  std::vector<shared_ptr<Target> > all_targets_;
  std::string yaml_file_path_;
  std::string camera_file_;
  std::string target_file_;
  CeresBlocks ceres_blocks_;                 /*!< This structure maintains the parameter sets for ceres */
  ros::ServiceServer start_server_;
  ros::ServiceServer observation_server_;
  ros::ServiceServer run_server_;
  ros::ServiceServer save_server_;
  ceres::Problem *P_;
  bool problem_initialized_;
  int total_observations_;
};// end of class icalServiceNode

int main(int argc, char** argv)
{
  ros::init(argc, argv, "ical_cal_service");
  ros::NodeHandle node_handle;
  icalServiceNode ISN(node_handle);
  ros::spin();
  ros::waitForShutdown();
  return 0;
}