/*
 * FrameFab.h
 * 
 * Created on:  April 7, 2017
 * Author:      Yijiang Huang, Thomas Cook
 * Institute:   MIT, Digital Structure Group, Building Tech
*/

#ifndef FRAMEFAB_PLANNER_H
#define FRAMEFAB_PLANNER_H

// boost::shared_ptr declaration
#include <boost/shared_ptr.hpp>

// ROS
#include <ros/ros.h>
#include <geometry_msgs/Pose.h>

// Moveit
#include <moveit/planning_scene_interface/planning_scene_interface.h>
#include <moveit/move_group_interface/move_group.h>

#include <moveit_visual_tools/moveit_visual_tools.h>

// framefab
#include <wire_frame/wire_frame_collision_objects.h>

namespace framefab
{

/*! @class FrameFab
 *  @brief The framefab main container class.
 *
 *  The framefab main container class. Coordinates the ROS interface
 *  and the data handling between the other classes.
*/
class FrameFabPlanner
{
  /* public functions */
 public:

  /*!
   *	@brief Constructor.
   * 	@param[in] ROS nodeHandle for parameters request
   */
  FrameFabPlanner(ros::NodeHandle &node_handle);

  /*!
   *	@brief Destructor.
   */
  ~FrameFabPlanner();

  /*
   * function for development, experiment goes here.
   */
  void testCartPlanning();
  void testDescartesPlanning();
  void setRobotHomePose();

  /* private functions */
 private:
  /*!
   * Reads and verifies the ROS parameters.
   * @return true if successful
   */
  bool readParameters();

  /*!
  * Performs the initialization procedure.
  * @return true if successful.
  */
  bool initialize() {}

  void makePlan(const std::vector<geometry_msgs::Pose> &way_points){}

  /* private data */
 private:

  //! ROS nodehandle.
  ros::NodeHandle &node_handle_;

  moveit::planning_interface::PlanningSceneInterfacePtr ptr_planning_scene_interface_;
  move_group_interface::MoveGroupPtr              ptr_move_group_;
  wire_frame::WireFrameCollisionObjectsPtr        ptr_wire_frame_collision_objects_;

  moveit_visual_tools::MoveItVisualToolsPtr ptr_moveit_visual_tools;
};

typedef boost::shared_ptr<FrameFabPlanner>        FrameFabPlannerPtr;
typedef boost::shared_ptr<const FrameFabPlanner>  FrameFabPlannerConstaPtr;

} /* namespace */

#endif // FRAMEFAB_PLANNER_H