#include <stdlib.h>
#include <stdio.h>
#include <math.h>
#include <vector>
#include <map>
#include <string>
#include <iostream>
#include <random>
#include <Eigen/Geometry>
#include <time.h>
#include <chrono>
#include <ros/ros.h>
#include <std_srvs/Empty.h>
#include <sensor_msgs/JointState.h>
#include <control_msgs/JointTrajectoryControllerState.h>
#include <iiwa_robot_controllers/PositionCommand.h>
#include <iiwa_robot_controllers/FRICommand.h>
#include <iiwa_robot_controllers/SetInjectedError.h>
#include <iiwa_robot_controllers/iiwa_robot_config.hpp>
#include <arc_utilities/pretty_print.hpp>
#include <arc_utilities/eigen_helpers.hpp>

class IIWARobotPositionInterface
{
protected:

    std::vector<std::string> joint_names_;
    std::map<std::string, iiwa_robot_controllers::JointLimits> joint_limits_;
    std::map<std::string, iiwa_robot_controllers::PIDParams> joint_controller_params_;

    std::mt19937_64 rng_;
    arc_helpers::TruncatedNormalDistribution actuator_noise_distribution_;
    std::map<std::string, double> joint_uncertainty_params_;

    bool target_config_valid_;
    bool current_config_valid_;
    std::vector<double> target_config_;
    std::vector<double> current_config_;
    std::vector<double> current_velocities_;

    std::vector<double> config_error_integrals_;
    std::vector<double> last_config_errors_;

    ros::NodeHandle nh_;
    ros::Publisher status_pub_;
    ros::Publisher command_pub_;
    ros::Subscriber feedback_sub_;
    ros::Subscriber config_target_sub_;
    ros::ServiceServer abort_server_;
    ros::ServiceServer uncertainty_update_server_;

public:

    IIWARobotPositionInterface(ros::NodeHandle& nh,
                               const std::mt19937_64& rng,
                               const std::string& position_command_topic,
                               const std::string& config_feedback_topic,
                               const std::string& fri_command_topic,
                               const std::string& status_topic,
                               const std::string& abort_service,
                               const std::string& uncertainty_update_service,
                               const std::map<std::string, iiwa_robot_controllers::JointLimits>& joint_limits,
                               const std::map<std::string, iiwa_robot_controllers::PIDParams>& joint_controller_params,
                               const std::map<std::string, double>& joint_uncertainty_params) : rng_(rng), actuator_noise_distribution_(0.0, 0.5, -1.0, 1.0), nh_(nh)
    {

        target_config_valid_ = false;
        current_config_valid_ = false;
        target_config_ = std::vector<double>();
        current_config_ = std::vector<double>();
        current_velocities_ = std::vector<double>();
        assert(SetsEqual(arc_helpers::GetKeys(joint_limits), arc_helpers::GetKeys(joint_controller_params)));
        assert(SetsEqual(arc_helpers::GetKeys(joint_limits), arc_helpers::GetKeys(joint_uncertainty_params)));
        joint_names_ = arc_helpers::GetKeys(joint_limits);
        config_error_integrals_ = std::vector<double>(joint_names_.size(), 0.0);
        last_config_errors_ = std::vector<double>(joint_names_.size(), 0.0);
        joint_limits_ = joint_limits;
        joint_controller_params_ = joint_controller_params;
        joint_uncertainty_params_ = joint_uncertainty_params;
        status_pub_ = nh_.advertise<control_msgs::JointTrajectoryControllerState>(status_topic, 1, false);
        command_pub_ = nh_.advertise<iiwa_robot_controllers::FRICommand>(fri_command_topic, 1, false);
        feedback_sub_ = nh_.subscribe(config_feedback_topic, 1, &IIWARobotPositionInterface::ConfigFeedbackCallback, this);
        config_target_sub_ = nh_.subscribe(position_command_topic, 1, &IIWARobotPositionInterface::ConfigTargetCallback, this);
        abort_server_ = nh_.advertiseService(abort_service, &IIWARobotPositionInterface::AbortCB, this);
        uncertainty_update_server_ = nh_.advertiseService(uncertainty_update_service, &IIWARobotPositionInterface::UncertaintyUpdateService, this);
    }

    void Loop(const double control_rate)
    {
        const double control_interval = 1.0 / control_rate;
        ros::Rate spin_rate(control_rate);
        uint8_t iteration_count = 0x00;
        while (ros::ok())
        {
            // Process callbacks
            ros::spinOnce();
            // Run controller
            if (iteration_count == 0x00)
            {
                if (target_config_valid_ && current_config_valid_)
                {
                    const std::vector<double> raw_config_correction = ComputeRawNextStep(current_config_, target_config_, control_interval);
                    const std::vector<double> real_config_correction = LimitCorrectionVelocities(raw_config_correction, joint_names_, joint_limits_);
                    CommandVelocities(current_config_, real_config_correction, control_interval);
                    PublishState(target_config_, real_config_correction, current_config_, current_velocities_);
                }
                else if (current_config_valid_)
                {
                    //const std::vector<double> zero_velocity(joint_names_.size(), 0.0);
                    //CommandVelocities(zero_velocity);
                    //PublishState(current_config_, zero_velocity, current_config_, current_velocities_);
                    PublishState(current_config_, std::vector<double>(), current_config_, current_velocities_);
                }
                iteration_count = 0x00;
            }
            else
            {
                iteration_count++;
            }
            // Spin
            spin_rate.sleep();
        }
    }

    inline void PublishState(const std::vector<double>& target_config, const std::vector<double>& target_velocities, const std::vector<double>& current_config, const std::vector<double>& current_velocities)
    {
        control_msgs::JointTrajectoryControllerState state_msg;
        state_msg.joint_names = joint_names_;
        state_msg.desired.positions = target_config;
        state_msg.desired.velocities = target_velocities;
        state_msg.actual.positions = current_config;
        state_msg.actual.velocities = current_velocities;
        state_msg.error.positions = EigenHelpers::Sub(target_config, current_config);
        state_msg.error.velocities = EigenHelpers::Sub(target_velocities, current_velocities);
        status_pub_.publish(state_msg);
    }

    inline bool AbortCB(std_srvs::Empty::Request& req, std_srvs::Empty::Response& res)
    {
        UNUSED(req);
        UNUSED(res);
        ROS_INFO("Aborting config target");
        target_config_valid_ = false;
        config_error_integrals_ = std::vector<double>(joint_names_.size(), 0.0);
        last_config_errors_ = std::vector<double>(joint_names_.size(), 0.0);
        return true;
    }

    inline bool UncertaintyUpdateService(iiwa_robot_controllers::SetInjectedError::Request& req, iiwa_robot_controllers::SetInjectedError::Response& res)
    {
        UNUSED(res);
        if (req.actuator_name.size() != req.actuator_error.size())
        {
            ROS_WARN_NAMED(ros::this_node::getName(), "Invalid noise parameter update, %zu actuator names, %zu actuator errors", req.actuator_name.size(), req.actuator_error.size());
            return true;
        }
        std::map<std::string, double> joint_actuator_error_map;
        for (size_t idx = 0; idx < req.actuator_name.size(); idx++)
        {
            const std::string& actuator_name = req.actuator_name[idx];
            const double actuator_error = req.actuator_error[idx];
            joint_actuator_error_map[actuator_name] = actuator_error;
        }
        for (size_t idx =0; idx < joint_names_.size(); idx++)
        {
            const std::string& joint_name = joint_names_[idx];
            const double joint_error = arc_helpers::RetrieveOrDefault(joint_actuator_error_map, joint_name, 0.0);
            joint_uncertainty_params_[joint_name] = joint_error;
        }
        return true;
    }

    inline static double ClampValue(const double value, const double lower_limit, const double upper_limit)
    {
        return std::max(lower_limit, std::min(upper_limit, value));
    }

    inline std::vector<double> ComputeConfigError(const std::vector<double>& current_config, const std::vector<double>& target_config) const
    {
        const std::vector<double> config_error = EigenHelpers::Sub(target_config, current_config);
        return config_error;
    }

    inline std::vector<double> ComputeNewConfigErrorIntegral(const std::vector<double>& raw_config_error_integral, const std::vector<std::string>& joint_names, const std::map<std::string, iiwa_robot_controllers::PIDParams>& joint_params) const
    {
        assert(raw_config_error_integral.size() == joint_names.size());
        std::vector<double> limited_config_error_integral(raw_config_error_integral.size(), 0.0);
        for (size_t idx = 0; idx < limited_config_error_integral.size(); idx++)
        {
            const double joint_config_error_integral = raw_config_error_integral[idx];
            const std::string& joint_name = joint_names[idx];
            const auto found_itr = joint_params.find(joint_name);
            assert(found_itr != joint_params.end());
            const double i_clamp = found_itr->second.Iclamp();
            const double clamped_joint_config_error_integral = ClampValue(joint_config_error_integral, -i_clamp, i_clamp);
            limited_config_error_integral[idx] = clamped_joint_config_error_integral;
        }
        return limited_config_error_integral;
    }

    inline std::vector<double> ComputeRawNextStep(const std::vector<double>& current_config, const std::vector<double>& target_config, const double time_interval)
    {
        assert(joint_names_.size() == current_config.size());
        assert(joint_names_.size() == target_config.size());
        //std::cout << "Current config: " << PrettyPrint::PrettyPrint(current_config) << std::endl;
        //std::cout << "Target config: " << PrettyPrint::PrettyPrint(target_config) << std::endl;
        // Compute the pose error in our 'world frame'
        const std::vector<double> config_errors = ComputeConfigError(current_config, target_config);
        //std::cout << "Current errors: " << PrettyPrint::PrettyPrint(config_errors) << std::endl;
        // Compute the integral of pose error & update the stored value
        const std::vector<double> config_error_integrals_update = EigenHelpers::Add(EigenHelpers::Multiply(last_config_errors_, 0.5), EigenHelpers::Multiply(config_errors, 0.0));
        config_error_integrals_ = ComputeNewConfigErrorIntegral(EigenHelpers::Add(config_error_integrals_, config_error_integrals_update), joint_names_, joint_controller_params_);
        // Compute the derivative of pose error
        const std::vector<double> config_error_derivatives = EigenHelpers::Divide(EigenHelpers::Sub(config_errors, last_config_errors_), time_interval);
        // Update the stored pose error
        last_config_errors_ = config_errors;
        // Compute Feedback terms
        std::vector<double> feedback_terms(config_errors.size(), 0.0);
        for (size_t idx = 0; idx < feedback_terms.size(); idx++)
        {
            const double joint_config_error = config_errors[idx];
            const double joint_config_error_integral = config_error_integrals_[idx];
            const double joint_config_error_derivative = config_error_derivatives[idx];
            const std::string& joint_name = joint_names_[idx];
            const auto found_itr = joint_controller_params_.find(joint_name);
            assert(found_itr != joint_controller_params_.end());
            const iiwa_robot_controllers::PIDParams& params = found_itr->second;
            const double p_term = joint_config_error * params.Kp();
            const double i_term = joint_config_error_integral * params.Ki();
            const double d_term = joint_config_error_derivative * params.Kd();
            const double feedback_term = p_term + i_term + d_term;
            feedback_terms[idx] = feedback_term;
        }
        //std::cout << "Feedback command: " << PrettyPrint::PrettyPrint(feedback_terms) << std::endl;
        return feedback_terms;
    }

    inline std::vector<double> LimitCorrectionVelocities(const std::vector<double>& raw_velocities, const std::vector<std::string>& joint_names, const std::map<std::string, iiwa_robot_controllers::JointLimits>& limits) const
    {
        assert(raw_velocities.size() == joint_names.size());
        std::vector<double> limited_velocities(raw_velocities.size(), 0.0);
        for (size_t idx = 0; idx < limited_velocities.size(); idx++)
        {
            const double raw_velocity = raw_velocities[idx];
            const std::string& joint_name = joint_names[idx];
            const auto found_itr = limits.find(joint_name);
            assert(found_itr != limits.end());
            const double velocity_limit = found_itr->second.MaxVelocity();
            const double limited_velocity = ClampValue(raw_velocity, -velocity_limit, velocity_limit);
            limited_velocities[idx] = limited_velocity;
        }
        return limited_velocities;
    }

    inline double ApplyNoise(const double control_input, const double noise_bound)
    {
        const double noise = actuator_noise_distribution_(rng_) * (noise_bound * control_input);
        // Combine noise with control input
        const double noisy_control_input = control_input + noise;
        return noisy_control_input;
    }

    inline std::vector<double> GetNoisyVelocities(const std::vector<double>& raw_velocities)
    {
        assert(raw_velocities.size() == joint_names_.size());
        std::vector<double> noisy_velocities(raw_velocities.size(), 0.0);
        for (size_t idx = 0; idx < joint_names_.size(); idx++)
        {
            const std::string& joint_name = joint_names_[idx];
            const double raw_velocity = raw_velocities[idx];
            const double actuator_noise = joint_uncertainty_params_.find(joint_name)->second;
            const double noisy_velocity = ApplyNoise(raw_velocity, actuator_noise);
            noisy_velocities[idx] = noisy_velocity;
        }
        return noisy_velocities;
    }

    inline void CommandVelocities(const std::vector<double>& current_config, const std::vector<double>& velocities, const double control_interval)
    {
        const std::vector<double> noisy_velocities = GetNoisyVelocities(velocities);
        const std::vector<double> deltas = EigenHelpers::Multiply(noisy_velocities, control_interval);
        const std::vector<double> new_target_config = EigenHelpers::Add(current_config, deltas);
        iiwa_robot_controllers::FRICommand position_command;
        position_command.mode = iiwa_robot_controllers::FRICommand::POSITION;
        position_command.joint_name = joint_names_;
        position_command.joint_command = new_target_config;
        command_pub_.publish(position_command);
    }

    inline bool IsSubset(const std::vector<std::string>& candidate_set, const std::vector<std::string>& candidate_subset) const
    {
        if (candidate_set.size() < candidate_subset.size())
        {
            return false;
        }
        std::map<std::string, uint8_t> names_map;
        for (size_t idx = 0; idx < candidate_set.size(); idx++)
        {
            const std::string& name = candidate_set[idx];
            names_map[name] = 0x01;
        }
        for (size_t idx = 0; idx < candidate_subset.size(); idx++)
        {
            const std::string& name = candidate_subset[idx];
            const auto found_itr = names_map.find(name);
            if (found_itr == names_map.end())
            {
                return false;
            }
        }
        return true;
    }

    inline bool SetsEqual(const std::vector<std::string>& set1, const std::vector<std::string>& set2) const
    {
        if (IsSubset(set1, set2) && IsSubset(set2, set1))
        {
            return true;
        }
        else
        {
            return false;
        }
    }

    inline void ConfigTargetCallback(iiwa_robot_controllers::PositionCommand config_target)
    {
        if (config_target.name.size() == config_target.command.size())
        {
            // Push the command into a map
            std::map<std::string, double> command_map;
            for (size_t idx = 0; idx < config_target.name.size(); idx++)
            {
                const std::string& name = config_target.name[idx];
                const double command = config_target.command[idx];
                command_map[name] = command;
            }
            // Extract the joint commands in order
            std::vector<double> target_config(joint_names_.size(), 0.0);
            bool command_valid = true;
            for (size_t idx = 0; idx < joint_names_.size(); idx++)
            {
                // Get the name of the joint
                const std::string& joint_name = joint_names_[idx];
                // Get the commanded value
                const auto found_itr = command_map.find(joint_name);
                if (found_itr != command_map.end())
                {
                    const double position = found_itr->second;
                    // Get the limits for the joint
                    const auto limits_found_itr = joint_limits_.find(joint_name);
                    // If we have limits saved, limit the joint command
                    if (limits_found_itr != joint_limits_.end())
                    {
                        const std::pair<double, double> limits = limits_found_itr->second.PositionLimits();
                        const double lower_limit = limits.first;
                        const double upper_limit = limits.second;
                        const double limited_position = ClampValue(position, lower_limit, upper_limit);
                        target_config[idx] = limited_position;
                    }
                    // If we don't have limits saved, then we don't need to limit the joint value
                    else
                    {
                        target_config[idx] = position;
                    }
                }
                else
                {
                    ROS_WARN("Invalid JointCommand: joint %s missing", joint_name.c_str());
                    command_valid = false;
                }
            }
            if (command_valid == true)
            {
                target_config_ = target_config;
                target_config_valid_ = true;
            }
        }
        else
        {
            ROS_WARN("Invalid JointCommand: %zu names, %zu commands", config_target.name.size(), config_target.command.size());
            target_config_valid_ = false;
        }
    }

    inline void ConfigFeedbackCallback(sensor_msgs::JointState config_feedback)
    {
        if ((config_feedback.name.size() == config_feedback.position.size()) && (config_feedback.name.size() == config_feedback.velocity.size()) && IsSubset(config_feedback.name, joint_names_))
        {
            // Push the joint state into a map
            std::map<std::string, std::pair<double, double>> joint_state_map;
            for (size_t idx = 0; idx < config_feedback.name.size(); idx++)
            {
                const std::string& name = config_feedback.name[idx];
                const double position = config_feedback.position[idx];
                const double velocity = config_feedback.velocity[idx];
                joint_state_map[name] = std::make_pair(position, velocity);
            }
            // Extract the joint state in order
            std::vector<double> current_config(joint_names_.size(), 0.0);
            std::vector<double> current_velocities(joint_names_.size(), 0.0);
            bool config_valid = true;
            for (size_t idx = 0; idx < joint_names_.size(); idx++)
            {
                const std::string& joint_name = joint_names_[idx];
                const auto found_itr = joint_state_map.find(joint_name);
                if (found_itr != joint_state_map.end())
                {
                    const double position = found_itr->second.first;
                    const double velocity = found_itr->second.second;
                    current_config[idx] = position;
                    current_velocities[idx] = velocity;
                }
                else
                {
                    ROS_WARN("Invalid JointState feedback: joint %s missing", joint_name.c_str());
                    config_valid = false;
                }
            }
            if (config_valid == true)
            {
                current_config_ = current_config;
                current_velocities_ = current_velocities;
                current_config_valid_ = true;
            }
        }
        else
        {
            ROS_WARN("Invalid JointState feedback: %zu names, %zu positions", config_feedback.name.size(), config_feedback.position.size());
            current_config_valid_ = false;
        }
    }
};

int main(int argc, char** argv)
{
    ros::init(argc, argv, "iiwa_position_controller");
    ROS_INFO("Starting iiwa_position_controller...");
    ros::NodeHandle nh;
    ros::NodeHandle nhp("~");
    const std::string DEFAULT_POSITION_COMMAND_TOPIC = "joint_command_position";
    const std::string DEFAULT_STATUS_TOPIC = "arm_position_controller/status";
    const std::string DEFAULT_CONFIG_FEEDBACK_TOPIC = "joint_states";
    const std::string DEFAULT_FRI_COMMAND_TOPIC = "iiwa_FRI_command";
    const std::string DEFAULT_ABORT_SERVICE = "arm_position_controller/abort";
    const std::string DEFAULT_UNCERTAINTY_UPDATE_SERVICE = "arm_position_controller/set_error";
    const std::string DEFAULT_JOINT_NAME_PREFIX = "iiwa";
    const double DEFAULT_CONTROL_RATE = 500.0; //25.0;
    const double DEFAULT_POSITION_LIMIT_SCALING = 0.95;
    const double DEFAULT_VELOCITY_LIMIT_SCALING = 0.05;
    std::string position_command_topic;
    std::string fri_command_topic;
    std::string config_feedback_topic;
    std::string status_topic;
    std::string abort_service;
    std::string uncertainty_update_service;
    std::string joint_name_prefix;
    double control_rate = DEFAULT_CONTROL_RATE;
    double position_limit_scaling = DEFAULT_POSITION_LIMIT_SCALING;
    double velocity_limit_scaling = DEFAULT_VELOCITY_LIMIT_SCALING;
    nhp.param(std::string("position_command_topic"), position_command_topic, DEFAULT_POSITION_COMMAND_TOPIC);
    nhp.param(std::string("config_feedback_topic"), config_feedback_topic, DEFAULT_CONFIG_FEEDBACK_TOPIC);
    nhp.param(std::string("fri_command_topic"), fri_command_topic, DEFAULT_FRI_COMMAND_TOPIC);
    nhp.param(std::string("status_topic"), status_topic, DEFAULT_STATUS_TOPIC);
    nhp.param(std::string("abort_service"), abort_service, DEFAULT_ABORT_SERVICE);
    nhp.param(std::string("uncertainty_update_service"), uncertainty_update_service, DEFAULT_UNCERTAINTY_UPDATE_SERVICE);
    nhp.param(std::string("joint_name_prefix"), joint_name_prefix, DEFAULT_JOINT_NAME_PREFIX);
    nhp.param(std::string("control_rate"), control_rate, DEFAULT_CONTROL_RATE);
    nhp.param(std::string("position_limit_scaling"), position_limit_scaling, DEFAULT_POSITION_LIMIT_SCALING);
    nhp.param(std::string("velocity_limit_scaling"), velocity_limit_scaling, DEFAULT_VELOCITY_LIMIT_SCALING);
    const double real_position_limit_scaling = std::max(0.0, std::min(1.0, std::abs(position_limit_scaling)));
    const double real_velocity_limit_scaling = std::max(0.0, std::min(1.0, std::abs(velocity_limit_scaling)));
    // Joint limits
    const std::map<std::string, iiwa_robot_controllers::JointLimits> joint_limits = iiwa_robot_controllers::GetArmLimits(joint_name_prefix, real_position_limit_scaling, real_velocity_limit_scaling);
    // Joint PID params
    const std::map<std::string, iiwa_robot_controllers::PIDParams> joint_controller_params = iiwa_robot_controllers::GetArmDefaultPositionControllerParams(joint_name_prefix);
    // Make a default zero uncertainty
    const std::vector<std::string> joint_names = arc_helpers::GetKeys(joint_limits);
    const std::map<std::string, double> joint_uncertainty_params = arc_helpers::MakeFromKeysAndValues(joint_names, std::vector<double>(joint_names.size(), 0.0));
    // Prepare the default RNG
    const auto seed = std::chrono::high_resolution_clock::now().time_since_epoch().count();
    std::mt19937_64 prng(seed);
    IIWARobotPositionInterface controller(nh, prng, position_command_topic, config_feedback_topic, fri_command_topic, status_topic, abort_service, uncertainty_update_service, joint_limits, joint_controller_params, joint_uncertainty_params);
    ROS_INFO("...startup complete");
    controller.Loop(control_rate);
    return 0;
}
