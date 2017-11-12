//
// Created by yijiangh on 7/8/17.
//
#include <ros/console.h>

#include <Eigen/Geometry>

#include "generate_motion_plan.h"
#include "trajectory_utils.h"
#include "common_utils.h"
#include <descartes_trajectory/axial_symmetric_pt.h>
#include <descartes_trajectory/joint_trajectory_pt.h>

#include <descartes_planner/ladder_graph_dag_search_lazy_collision.h>
#include <descartes_planner/ladder_graph_dag_search.h>
#include <descartes_planner/dense_planner.h>
#include <descartes_planner/graph_builder.h>
#include <moveit/planning_scene/planning_scene.h>

// for serializing ladder graph
#include <descartes_parser/descartes_parser.h>

// for immediate execution
#include <actionlib/client/simple_action_client.h>
#include <control_msgs/FollowJointTrajectoryAction.h>

// msg
#include <geometry_msgs/Pose.h>
#include <trajectory_msgs/JointTrajectoryPoint.h>
#include <framefab_msgs/SubProcess.h>

// srv
#include <moveit_msgs/ApplyPlanningScene.h>

#include <eigen_conversions/eigen_msg.h>

#define SANITY_CHECK(cond) do { if ( !(cond) ) { throw std::runtime_error(#cond); } } while (false);

namespace{ //util function namespace

void constructPlanningScenes(moveit::core::RobotModelConstPtr moveit_model,
                             const std::vector<moveit_msgs::CollisionObject>& collision_objs,
                             std::vector<planning_scene::PlanningScenePtr>& planning_scenes)
{
  const auto build_scenes_start = ros::Time::now();

  planning_scenes.clear();
  planning_scenes.reserve(collision_objs.size());

  planning_scene::PlanningScenePtr root_scene(new planning_scene::PlanningScene(moveit_model));
  root_scene->getCurrentStateNonConst().setToDefaultValues();
  root_scene->getCurrentStateNonConst().update();
  planning_scenes.push_back(root_scene);

  for (std::size_t i = 0; i < collision_objs.size() - 1; ++i) // We use all but the last collision object
  {
    auto last_scene = planning_scenes.back();
    auto child = last_scene->diff();

    if (!child->processCollisionObjectMsg(collision_objs[i]))
    {
      ROS_WARN("[Process Planning] Failed to process collision object");
    }
    planning_scenes.push_back(child);
  }

  const auto build_scenes_end = ros::Time::now();
  ROS_INFO_STREAM("[Process Planning] constructing planning scenes took " << (build_scenes_end-build_scenes_start).toSec()
                                                                          << " seconds.");
}

void constructLadderGraphSegments(descartes_core::RobotModelPtr model,
                                  moveit::core::RobotModelConstPtr moveit_model,
                                  std::vector<descartes_planner::ConstrainedSegment>& segs,
                                  const std::vector<planning_scene::PlanningScenePtr>& planning_scenes,
                                  const std::string& move_group_name,
                                  const std::vector<std::string> &joint_names,
                                  std::vector<descartes_planner::LadderGraph>& graphs,
                                  std::vector<std::size_t>& graph_indices)
{
  auto graph_build_start = ros::Time::now();

  graphs.clear();
  graphs.reserve(segs.size());

  for (std::size_t i = 0; i < segs.size(); ++i)
  {
    model->setPlanningScene(planning_scenes[i]);
    if (true)
    {
      descartes_planner::ConstrainedSegment &seg = segs[i];
      ROS_INFO_STREAM("[Process Planning] process #" << i << ", orient size: " << seg.orientations.size());
    }
    graphs.push_back(descartes_planner::sampleConstrainedPaths(*model, segs[i]));
    graph_indices.push_back(graphs.back().size());
  }

  auto graph_build_end = ros::Time::now();
  ROS_INFO_STREAM("[Process Planning] Ladder Graph building took: "
                      << (graph_build_end - graph_build_start).toSec() << " seconds");
}

void appendLadderGraphSegments(std::vector<descartes_planner::LadderGraph>& graphs,
                                descartes_planner::LadderGraph& final_graph)
{
  const auto append_start = ros::Time::now();

  int append_id = 0;
  for (const auto& graph : graphs)
  {
    ROS_INFO_STREAM("[Process Planning] appending graph #" << append_id);
    descartes_planner::appendInTime(final_graph, graph);
    append_id++;
  }

  const auto append_end = ros::Time::now();
  ROS_INFO_STREAM("[Process Planning] Graph appending took: " << (append_end - append_start).toSec() << " seconds");
}

void searchLadderGraphforProcessROSTraj(descartes_core::RobotModelPtr model,
                                        descartes_planner::LadderGraph& final_graph,
                                        std::vector<std::size_t>& graph_indices,
                                        std::vector<descartes_planner::ConstrainedSegment>& segs,
                                        std::vector<framefab_msgs::UnitProcessPlan>& plans)
{
  const auto search_start = ros::Time::now();
  descartes_planner::DAGSearch search(final_graph);
  double cost = search.run();

  const auto search_end = ros::Time::now();
  ROS_INFO_STREAM("[Process Planning] DAG Search took " << (search_end-search_start).toSec()
                                                        << " seconds and produced a result with dist = " << cost);


  // Step 4 : Harvest shortest path in graph to retract trajectory
  auto path_idxs = search.shortestPath();
  framefab_process_planning::DescartesTraj sol;
  for (size_t j = 0; j < path_idxs.size(); ++j)
  {
    const auto idx = path_idxs[j];
    const auto* data = final_graph.vertex(j, idx);
    const auto& tm = final_graph.getRung(j).timing;
    auto pt = descartes_core::TrajectoryPtPtr(new descartes_trajectory::JointTrajectoryPt(
        std::vector<double>(data, data + 6), tm));
    sol.push_back(pt);
  }

  trajectory_msgs::JointTrajectory ros_traj = framefab_process_planning::toROSTrajectory(sol, *model);

  auto it = ros_traj.points.begin();
  for(size_t i = 0; i < segs.size(); i++)
  {
    framefab_msgs::SubProcess sub_process;

    sub_process.process_type = framefab_msgs::SubProcess::PROCESS;
    sub_process.main_data_type = framefab_msgs::SubProcess::CART;
    sub_process.joint_array.points =  std::vector<trajectory_msgs::JointTrajectoryPoint>(it, it + graph_indices[i]);

    plans[i].sub_process_array.push_back(sub_process);

    it = it + graph_indices[i];
  }
}

void transitionPlanning(std::vector<framefab_msgs::UnitProcessPlan>& plans,
                        moveit::core::RobotModelConstPtr moveit_model,
                        ros::ServiceClient& planning_scene_diff_client,
                        const std::string& move_group_name,
                        const std::vector<double>& start_state,
                        std::vector<planning_scene::PlanningScenePtr>& planning_scenes)
{
  if (plans.size() == 0)
  {
    ROS_ERROR("[transionPlanning] plans size = 0!");
    assert(false);
  }

  const auto tr_planning_start = ros::Time::now();

  std::vector<double> last_joint_pose = start_state;
  std::vector<double> current_first_joint_pose;

  for(size_t i = 0; i < plans.size(); i++)
  {
    ROS_INFO_STREAM("[Transition Planning] process #" << i);

    if(0 != i)
    {
      last_joint_pose = plans[i-1].sub_process_array.back().joint_array.points.back().positions;
    }

    current_first_joint_pose = plans[i].sub_process_array.back().joint_array.points.front().positions;

    if(last_joint_pose == current_first_joint_pose)
    {
      // skip transition planning
      continue;
    }

    // update the planning scene
    if(!planning_scene_diff_client.waitForExistence())
    {
      ROS_ERROR_STREAM("[Tr Planning] cannot connect with planning scene diff server...");
    }

    moveit_msgs::ApplyPlanningScene srv;
    planning_scenes[i]->getPlanningSceneMsg(srv.request.scene);
    if(!planning_scene_diff_client.call(srv))
    {
      ROS_ERROR_STREAM("[Tr Planning] Failed to publish planning scene diff srv!");
    }

    trajectory_msgs::JointTrajectory ros_trans_traj =
        framefab_process_planning::getMoveitTransitionPlan(move_group_name,
                                                           last_joint_pose,
                                                           current_first_joint_pose,
                                                           start_state,
                                                           moveit_model);

    framefab_msgs::SubProcess sub_process;

    sub_process.process_type = framefab_msgs::SubProcess::TRANSITION;
    sub_process.main_data_type = framefab_msgs::SubProcess::JOINT;
    sub_process.joint_array = ros_trans_traj;

    plans[i].sub_process_array.insert(plans[i].sub_process_array.begin(), sub_process);
  }

  const auto tr_planning_end = ros::Time::now();
  ROS_INFO_STREAM("[Process Planning] Transition Planning took " << (tr_planning_end-tr_planning_start).toSec()
                                                                 << " seconds.");
}

void adjustTrajectoryTiming(std::vector<framefab_msgs::UnitProcessPlan>& plans,
                            const std::vector<std::string> &joint_names)
{
  if (plans.size() == 0)
  {
    ROS_ERROR("[ProcessPlanning : adjustTrajectoryTiming] plans size = 0!");
    assert(false);
  }

  if (0 == plans[0].sub_process_array.size())
  {
    ROS_ERROR("[ProcessPlanning : adjustTrajectoryTiming] plans[0] doesn't have sub process!");
    assert(false);
  }

  framefab_process_planning::fillTrajectoryHeaders(joint_names, plans[0].sub_process_array[0].joint_array);
  auto last_filled_jts = plans[0].sub_process_array[0].joint_array;

  // inline function for append trajectory headers (adjust time frame)
  auto adjustTrajectoryHeaders = [](trajectory_msgs::JointTrajectory& last_filled_jts, framefab_msgs::SubProcess& sp)
  {
    framefab_process_planning::appendTrajectoryHeaders(last_filled_jts, sp.joint_array, 1.0);
    last_filled_jts = sp.joint_array;
  };

  for(size_t i = 0; i < plans.size(); i++)
  {
    for (size_t j = 0; j < plans[i].sub_process_array.size(); j++)
    {
      adjustTrajectoryHeaders(last_filled_jts, plans[i].sub_process_array[j]);
    }
  }
}

} // end util namespace

bool framefab_process_planning::generateMotionPlan(
    descartes_core::RobotModelPtr model,
    std::vector<descartes_planner::ConstrainedSegment>& segs,
    const std::vector<moveit_msgs::CollisionObject>& collision_objs,
    moveit::core::RobotModelConstPtr moveit_model,
    ros::ServiceClient& planning_scene_diff_client,
    const std::string& move_group_name,
    const std::vector<double>& start_state,
    std::vector<framefab_msgs::UnitProcessPlan>& plans)
{
  // Step 0: Sanity checks
  if (segs.size() == 0)
  {
    ROS_ERROR("[Process Planning] input descartes Constrained Segment size = 0!");
    return false;
  }

  plans.resize(segs.size());
  const std::vector<std::string> &joint_names =
      moveit_model->getJointModelGroup(move_group_name)->getActiveJointModelNames();

  // Step 1: Let's create all of our planning scenes to collision check against
  std::vector<planning_scene::PlanningScenePtr> planning_scenes;
  constructPlanningScenes(moveit_model, collision_objs, planning_scenes);

  // Step 2: sample graph for each segment separately
  std::vector<descartes_planner::LadderGraph> graphs;
  std::vector<std::size_t> graph_indices;
  constructLadderGraphSegments(model, moveit_model, segs, planning_scenes,
                               move_group_name, joint_names,
                               graphs, graph_indices);

  // Step 2': save graph to msgs
  const auto save_graph_start = ros::Time::now();

  auto graph_list_msg = descartes_parser::convertToLadderGraphMsg(graphs);

  const auto save_graph_end = ros::Time::now();
  ROS_INFO_STREAM("[Process Planning] ladder graph saving took " << (save_graph_end-save_graph_start).toSec()
                                                                 << " seconds.");

  const auto parse_graph_start = ros::Time::now();

  graphs = descartes_parser::convertToLadderGraphList(graph_list_msg);

  const auto parse_graph_end = ros::Time::now();
  ROS_INFO_STREAM("[Process Planning] ladder graph parsing took " << (parse_graph_end-parse_graph_start).toSec()
                                                                  << " seconds.");

  // Step 3: append individual graph together to an unified one
  descartes_planner::LadderGraph final_graph(model->getDOF());
  appendLadderGraphSegments(graphs, final_graph);

  // Next, we build a search for the whole problem
  searchLadderGraphforProcessROSTraj(model, final_graph, graph_indices, segs, plans);

  // Step 5 : Plan for transition between each pair of sequential path
  transitionPlanning(plans, moveit_model, planning_scene_diff_client, move_group_name,
                     start_state, planning_scenes);

  // Step 6 : TODO: Process each transition plan to extract "near-process" segmentation

  // Step 7 : fill in trajectory's time headers and pack into sub_process_plans
  // for each unit_process
  adjustTrajectoryTiming(plans, joint_names);

  ROS_INFO("[Process Planning] trajectories solved and packing finished");
  return true;
}