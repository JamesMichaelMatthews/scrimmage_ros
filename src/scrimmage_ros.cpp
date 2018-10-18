#include <scrimmage_ros/scrimmage_ros.h>
#include <scrimmage/parse/ParseUtils.h>

#include <iostream>
#include <memory>
#include <array>
#include <stdexcept>
#include <cstdio>
#include <stdexcept>

#include <XmlRpcValue.h>
#include <time.h>

#define BOOST_NO_CXX11_SCOPED_ENUMS
#include <boost/filesystem.hpp>
#undef BOOST_NO_CXX11_SCOPED_ENUMS

namespace sc = scrimmage;
namespace fs = boost::filesystem;
using std::cout;
using std::endl;

namespace scrimmage_ros {
scrimmage_ros::scrimmage_ros(ros::NodeHandle &nh, ros::NodeHandle &private_nh,
                             const std::string &node_name) :
    nh_(nh), private_nh_(private_nh), node_name_(node_name) {
}

bool scrimmage_ros::init(std::ostream &out) {
    if (not private_nh_.getParam("loop_rate_hz", loop_rate_hz_)) {
        out << node_name_ << ": missing ros param: loop_rate_hz." << endl;
    }

    std::string mission_file;
    if (not private_nh_.getParam("mission_file", mission_file)) {
        out << node_name_ << ": missing ros param: mission_file." << endl;
        return false;
    }

    std::string entity_tag;
    if (not private_nh_.getParam("entity_tag", entity_tag)) {
        out << node_name_ << ": missing ros param: entity_tag." << endl;
        return false;
    }

    std::string plugin_tags_str;
    if (not private_nh_.getParam("plugin_tags", plugin_tags_str)) {
        out << node_name_ << ": missing ros param: plugin_tags." << endl;
    }

    if (not private_nh_.getParam("entity_id", entity_id_)) {
        out << node_name_ << ": missing ros param: entity_id." << endl;
    }

    int max_contacts = 100;
    if (not private_nh_.getParam("max_contacts", max_contacts)) {
        out << node_name_ << ": missing ros param: max_contacts." << endl;
    }

    auto param_override_func = [&](std::map<std::string, std::string>& param_map) {
        for (auto &kv : param_map) {
            std::string resolved_param;
            if (private_nh_.searchParam(kv.first, resolved_param)) {
                XmlRpc::XmlRpcValue xmlrpc;
                if (private_nh_.getParam(resolved_param, xmlrpc)) {
                    if (xmlrpc.getType() == XmlRpc::XmlRpcValue::Type::TypeInvalid) {
                        out << node_name_ << ": Invalid XmlRpc param: " << resolved_param << endl;
                    } else if (xmlrpc.getType() == XmlRpc::XmlRpcValue::Type::TypeBoolean) {
                        kv.second = std::to_string(bool(xmlrpc));
                    } else if (xmlrpc.getType() == XmlRpc::XmlRpcValue::Type::TypeInt) {
                        kv.second = std::to_string(int(xmlrpc));
                    } else if (xmlrpc.getType() == XmlRpc::XmlRpcValue::Type::TypeDouble) {
                        kv.second = std::to_string(double(xmlrpc));
                    } else if (xmlrpc.getType() == XmlRpc::XmlRpcValue::Type::TypeString) {
                        kv.second = std::string(xmlrpc);
                    } else if (xmlrpc.getType() == XmlRpc::XmlRpcValue::Type::TypeDateTime) {
                        struct tm time = tm(xmlrpc);
                        kv.second = std::to_string(timegm(&time));
                    } else if (xmlrpc.getType() == XmlRpc::XmlRpcValue::Type::TypeBase64) {
                        out << node_name_ << ": Unsupported XmlRpc type (TypeBase64): " << resolved_param << endl;
                    } else if (xmlrpc.getType() == XmlRpc::XmlRpcValue::Type::TypeArray) {
                        out << node_name_ << ": Unsupported XmlRpc type (TypeArray): " << resolved_param << endl;
                    } else if (xmlrpc.getType() == XmlRpc::XmlRpcValue::Type::TypeStruct) {
                        out << node_name_ << ": Unsupported XmlRpc type (TypeSTruct): " << resolved_param << endl;
                    } else {
                        out << node_name_ << ": Can't parse XmlRpc param: " << resolved_param << endl;
                    }
                } else {
                    out << node_name_ << ": Failed to retrieve XMLRpc value for: " << resolved_param << endl;
                }
            }
        }
    };

    // Get the current ROS log directory
    if (not private_nh_.getParam("log_dir", ros_log_dir_)) {
        ros_log_dir_ = scrimmage_ros::exec_command("roslaunch-logs");
        ros_log_dir_.erase(std::remove(ros_log_dir_.begin(), ros_log_dir_.end(), '\n'), ros_log_dir_.end());
    }

    if (not fs::exists(fs::path(ros_log_dir_))) {
        out << node_name_ << ": ROS log directory doesn't exist: " << ros_log_dir_ << endl;
    }

    const bool create_entity =
        external_.create_entity(mission_file, entity_tag, plugin_tags_str,
                                entity_id_, max_contacts,
                                ros_log_dir_ + "/scrimmage",
                                param_override_func);
    if (create_entity) {
        external_.print_plugins(out);
    } else {
        out << node_name_ << ": failed to load plugins for " << entity_tag << endl;
    }

    // Setup the dynamic reconfigure callback
    dyn_reconf_f_ = boost::bind(&scrimmage_ros::dyn_reconf_cb, this, _1, _2);
    dyn_reconf_server_.setCallback(dyn_reconf_f_);

    return true;
}

bool scrimmage_ros::step(const double &t, std::ostream &out) {
    if (not external_.step(t)) {
        out << node_name_ << ": external step returned false." << endl;
        return false;
    }
    return true;
}

std::string scrimmage_ros::exec_command(const char* cmd) {
    std::array<char, 512> buffer;
    std::string result;
    std::unique_ptr<FILE, decltype(&pclose)> pipe(popen(cmd, "r"), pclose);
    if (!pipe) throw std::runtime_error("popen() failed!");
    while (!feof(pipe.get())) {
        if (fgets(buffer.data(), 512, pipe.get()) != nullptr) {
            result += buffer.data();
        }
    }
    return result;
}

const double & scrimmage_ros::loop_rate_hz() {
    return loop_rate_hz_;
}

void scrimmage_ros::dyn_reconf_cb(scrimmage_rosConfig &config, uint32_t level) {
    (void)level; // the level parameter is not being used

    // If the callback is being called with the "default" message, ignore the
    // message
    if (config.param_name == scrimmage_rosConfig::__getDefault__().param_name) {
        return;
    }

    try {
        switch(config.param_type) {
        case scrimmage_ros_int:
            external_.param_server()->set_param<int>(
                config.param_name, std::stoi(config.param_value));
            break;
        case scrimmage_ros_bool:
            external_.param_server()->set_param<bool>(
                config.param_name, sc::str2bool(config.param_value));
            break;
        case scrimmage_ros_double:
            external_.param_server()->set_param<double>(
                config.param_name, std::stod(config.param_value));
            break;
        case scrimmage_ros_float:
            external_.param_server()->set_param<float>(
                config.param_name, std::stof(config.param_value));
            break;
        case scrimmage_ros_string:
            external_.param_server()->set_param<std::string>(
                config.param_name, config.param_value);
            break;
        default:
            std::cout << "scrimmage_ros dynamic reconfigure warning" << endl;
            std::cout << "param_name: " << config.param_name << endl;
            std::cout << "param_type: " << config.param_type << endl;
            std::cout << "param_value: " << config.param_value << endl;
            break;
        }
    } catch(const std::invalid_argument &ia) {
        std::cerr << "scrimmage_ros dynamic reconfigure exception: " << endl;
        std::cerr << ia.what() << endl;
        std::cerr << "param_name: " << config.param_name << endl;
        std::cerr << "param_type: " << config.param_type << endl;
        std::cerr << "param_value: " << config.param_value << endl;
    }
}

} // scrimmage_ros
