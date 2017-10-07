#include "ros/ros.h"
#include "sensor_msgs/Image.h"

void DepthImageCallback(const sensor_msgs::Image::ConstPtr& msg)
{
  ROS_INFO("I heard: something");
}

int main(int argc, char **argv)
{
  ros::init(argc, argv, "bundle_fusion");
  ros::NodeHandle n;
  ros::Subscriber sub = n.subscribe("/camera/depth/image", 1, DepthImageCallback);
  ros::spin();
  return 0;
}
