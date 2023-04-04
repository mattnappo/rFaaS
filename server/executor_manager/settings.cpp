
#include <filesystem>
#include <regex>

#include <spdlog/spdlog.h>

#include <rdmalib/util.hpp>

#include "settings.hpp"

namespace rfaas::executor_manager {

  SandboxType sandbox_deserialize(std::string type)
  {
    static std::map<std::string, SandboxType> sandboxes = {
      {"process", SandboxType::PROCESS},
      {"docker", SandboxType::DOCKER},
      {"sarus", SandboxType::SARUS}
    };
    return sandboxes.at(type);
  }

  std::string sandbox_serialize(SandboxType type)
  {
    static std::map<SandboxType, std::string> sandboxes = {
      {SandboxType::PROCESS, "process"},
      {SandboxType::DOCKER, "docker"},
      {SandboxType::SARUS, "sarus"}
    };
    return sandboxes.at(type);
  }

  Settings Settings::deserialize(std::istream & in)
  {
    Settings settings{};
    cereal::JSONInputArchive archive_in(in);
    archive_in(cereal::make_nvp("config", settings));
    archive_in(cereal::make_nvp("executor", settings.exec));
    archive_in(cereal::make_nvp("sandbox-configuration", settings.sandboxes));

    // read RDMA device details
    rfaas::device_data * dev = rfaas::devices::instance().device(settings.rdma_device);
    if(!dev) {
      spdlog::error("Data for device {} not found!", settings.rdma_device);
      throw std::runtime_error{"Unknown device!"};
    }
    settings.device = dev;

    // executor options
    settings.exec.max_inline_data = dev->max_inline_data;
    settings.exec.recv_buffer_size = dev->default_receive_buffer_size;

    if (settings.exec.sandbox_type != SandboxType::PROCESS) {
      settings.exec.sandbox_config = &settings.sandboxes.at(settings.exec.sandbox_type);
    }
    // FIXME: should be sent with request
    if (settings.exec.sandbox_type == SandboxType::SARUS) {
      settings.exec.sandbox_user = settings.exec.sandbox_config->user;
      settings.exec.sandbox_name = settings.exec.sandbox_config->name;
      for(auto & mount : settings.exec.sandbox_config->mounts)
        std::cerr << mount << std::endl;
    }

    return settings;
  }

  void SandboxConfiguration::generate_args(std::vector<std::string> & args, const std::string & user) const
  {
    for(auto & dev : this->devices)
      args.emplace_back(rdmalib::impl::string_format("--device=%s", dev));

    for(auto & mount : this->mount_filesystem) {
      std::string user_partition{mount};
      user_partition = std::regex_replace(user_partition, std::regex{R"(\{user\})"}, user);
      args.emplace_back(rdmalib::impl::string_format("--mount=type=bind,source=%s,destination=%s", user_partition, user_partition));
    }

    for(auto & mount : this->mounts)
      args.emplace_back(rdmalib::impl::string_format("--mount=type=bind,source=%s,destination=%s", mount, mount));

    for(auto & [key, value] : this->env) {
      args.emplace_back("-e");
      args.emplace_back(rdmalib::impl::string_format("%s=%s", key, value));
    }

  }

  std::string SandboxConfiguration::get_executor_path() const
  {
    // Horrible hack - we need to get the location of the executor.
    // We assume that rFaaS is built on the shared filesystem that is mounted
    // in the container.
    // Furtheermore, since rFaaS is built in a single directory, we know
    // that executor should be located in the same directory as
    // executor_manager.

    // This works only on Linux!
    auto path = std::filesystem::canonical("/proc/self/exe").parent_path();
    return path / "executor";
  }

}

