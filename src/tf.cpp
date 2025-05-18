#include <opencv2/highgui/highgui.hpp>
#include <opencv2/opencv.hpp>

#include <fstream>
#include <signal.h>
#include <sstream>

#include <PHOENIX/Transformer/TransformTree.hpp>
#include <PHOENIX/Utils/Info/Info.hpp>

#include <eigen3/Eigen/Eigen>
#include <eigen3/Eigen/Geometry>

// 使用标准C++库处理文件路径，替代boost::filesystem
#include <filesystem>
// 使用nlohmann/json库，替代boost::json
#include <nlohmann/json.hpp>

#include "Application.hpp"
#include "Message.hpp"
#include "SocketServer.hpp"

// 添加命名空间引用

using json = nlohmann::json;
namespace fs = std::filesystem;

int main(int argc, char *argv[]) {
  // 忽略 SIGPIPE 信号, 防止因为发送消息时客户端断开连接导致服务器崩溃
  // SIGPIPE 信号会在向一个已经关闭的 socket 发送消息时产生, 默认行为是终止进程
  // 后续需要对发送消息的返回值进行判断, 防止因为发送消息失败导致服务器崩溃
  struct sigaction sa;
  sa.sa_handler = SIG_IGN;
  sigaction(SIGPIPE, &sa, 0);

  int port = 4399;
  if (argc > 1 && argv[1] != nullptr) {
    try {
      port = std::stoi(argv[1]);
    } catch (const std::exception &e) {
      ERROR("Invalid port number: " + std::string(argv[1]));
      ERROR("Using default port 4399");
    }
  } else {
    INFO("No port specified, using default port 4399");
  }

  SocketServer server(port);
  std::shared_ptr<SocketServer> server_ptr(&server);
  Application<SocketServer> app(server_ptr);

  // 坐标转换树
  PHOENIX::TransformTree tt;
  // 读取外部文件中的坐标转换信息
  fs::path p("../asset/tf.json");
  std::ifstream ifs(p);
  std::string jsonstr((std::istreambuf_iterator<char>(ifs)),
                      std::istreambuf_iterator<char>());
  json j = json::parse(jsonstr);

  auto transforms = j["transform"];
  for (auto &tf : transforms) {
    std::string from = tf["from"];
    std::string to = tf["to"];
    auto translation = tf["translation"];
    auto rotation = tf["rotation"];
    tt.addTransform(
        from.c_str(), to.c_str(),
        PHOENIX::Transform(
            Eigen::Vector3d(translation[0], translation[1], translation[2]),
            Eigen::Vector3d(rotation[2], rotation[1], rotation[0])));
  }

  // 设置消息处理函数
  server.set_on_message([&](int client, const char *message) {
    Message msg(message);
    unsigned char *m = app.receive_and_decode(msg);
    if (m != nullptr &&
        msg.get_messageType() ==
            Message::MessageType::TRANSFORM_REQUEST) { // 请求坐标转换消息
      std::string from =
          std::string(((Message::TransformRequestData *)m)->From);
      std::string to = std::string(((Message::TransformRequestData *)m)->To);
      sleep(1);
      INFO("Request transform from " + from + " to " + to);
      auto tf = tt.getTransform(from, to); // 获取坐标转换
      double transform_data[3 + 4] = {tf.translation.x(), tf.translation.y(),
                                      tf.translation.z(), tf.rotation.x(),
                                      tf.rotation.y(),    tf.rotation.z(),
                                      tf.rotation.w()};
      app.encode_and_send(Message::MessageType::TRANSFORM, 0,
                          (unsigned char *)(transform_data),
                          sizeof(transform_data), client);
      SUCCESS("Transform sent.");
    }
  });

  // 设置连接成功处理函数
  server.set_on_connect([&](int client) {
    if (client > 15) // 最多支持 16 个客户端
      app.disconnect(client);
    INFO("Client " + std::to_string(client) + " connected."); // 输出连接信息
    // 发送 Hello client 消息
    std::string msg("Hello client " + std::to_string(client) + "\n");
    app.encode_and_send(Message::MessageType::STRING_MSG, 0,
                        (unsigned char *)(msg.c_str()), msg.size(), client);
  });

  // 设置断开连接处理函数
  server.set_on_disconnect([&](int client) {
    INFO("Client " + std::to_string(client) + " disconnected.");
  });

  // 添加主动发送消息命令
  app.add_command("send_tf", [&](std::string args) {
    if (args.size() < 3) {
      WARNING("Usage: send_tf <client> <from> <to>");
      return;
    }

    std::istringstream iss(args);

    int client;
    std::string from, to;

    iss >> client >> from >> to;

    auto tf = tt.getTransform(from, to);
    double transform_data[3 + 4] = {tf.translation.x(), tf.translation.y(),
                                    tf.translation.z(), tf.rotation.x(),
                                    tf.rotation.y(),    tf.rotation.z(),
                                    tf.rotation.w()};
    app.encode_and_send(Message::MessageType::TRANSFORM, 0,
                        (unsigned char *)(transform_data),
                        sizeof(transform_data), client);
    SUCCESS("Transform sent.");
  });

  server.start();
  app.join();

  return 0;
}