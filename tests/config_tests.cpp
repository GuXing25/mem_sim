#include <algorithm>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>

#include "hbm_sim/config/document.hpp"

namespace {

void require(bool condition, const std::string &message) {
  if (!condition)
    throw std::runtime_error(message);
}

bool has_entry(const std::vector<hbm_sim::config::ConfigEntry> &entries,
               const std::string &section, const std::string &key,
               const std::string &value = {}) {
  return std::any_of(entries.begin(), entries.end(), [&](const auto &entry) {
    return entry.section == section && entry.key == key &&
           (value.empty() || entry.value == value);
  });
}

} // namespace

int main(int argc, char **argv) {
  try {
    if (argc != 2)
      throw std::runtime_error("usage: config_tests SOURCE_DIR");
    const std::string root = argv[1];
    const auto hbm = hbm_sim::config::load_document(root + "/configs/hbm.cfg");
    const auto lpddr =
        hbm_sim::config::load_document(root + "/configs/lpddr.cfg");
    require(hbm.sectioned && lpddr.sectioned,
            "master configs must use schema-v2 sections");

    // 配置库存是公开接口：两份标准主配置、四份验证集、四份完整用例和
    // 一份开发者参数集。目录中不得悄悄增加用途不明的 cfg。
    std::vector<std::string> config_files;
    for (const auto &item :
         std::filesystem::recursive_directory_iterator(root + "/configs")) {
      if (item.is_regular_file() && item.path().extension() == ".cfg") {
        config_files.push_back(
            std::filesystem::relative(item.path(), root + "/configs")
                .generic_string());
      }
    }
    std::sort(config_files.begin(), config_files.end());
    require(config_files ==
                std::vector<std::string>(
                    {"developer.cfg", "hbm.cfg", "lpddr.cfg",
                     "usecases/hbm.cfg", "usecases/hbm_nstacks.cfg",
                     "usecases/lpddr.cfg", "usecases/lpddr_nstacks.cfg",
                     "validation/hbm3.cfg", "validation/hbm4.cfg",
                     "validation/lpddr5.cfg", "validation/lpddr6.cfg"}),
            "config inventory must be 2 masters + 4 validation + 4 usecases + "
            "1 developer");

    // 每一个真正赋值的配置行必须带中文说明。这里不判断自然语言质量，但阻止
    // 后续新增无注释参数；section、空行和纯注释不受影响。
    for (const auto &relative : config_files) {
      std::ifstream input(root + "/configs/" + relative);
      require(static_cast<bool>(input),
              "cannot read config for comment check: " + relative);
      std::string line;
      int lineno = 0;
      while (std::getline(input, line)) {
        ++lineno;
        const auto first = line.find_first_not_of(" \t");
        if (first == std::string::npos || line[first] == '#' ||
            line[first] == '[' || line.find('=') == std::string::npos) {
          continue;
        }
        require(line.find('#', line.find('=') + 1) != std::string::npos,
                relative + ":" + std::to_string(lineno) +
                    " config value is missing an inline explanation");
      }
    }

    auto selection = hbm_sim::config::discover_selection({hbm}, {}, {});
    require(selection.standard == "hbm4", "HBM default selection must be HBM4");
    require(selection.preset.empty(),
            "standard master must not force a named preset");
    auto active = hbm_sim::config::resolve_document(hbm, selection);
    require(has_entry(active, "standard.hbm4", "supports_ecc", "true"),
            "HBM4 standard section was not activated");
    require(!has_entry(active, "standard.hbm3", "supports_ecc"),
            "inactive HBM3 section leaked into HBM4");
    require(has_entry(active, "phy", "mem_phy_mode", "behavioral"),
            "section-aware PHY key alias was not resolved");

    selection = hbm_sim::config::discover_selection({hbm}, "hbm3", {});
    active = hbm_sim::config::resolve_document(hbm, selection);
    require(has_entry(active, "standard.hbm3", "supports_ecc", "false"),
            "CLI HBM3 selection did not activate HBM3");
    require(!has_entry(active, "standard.hbm4", "supports_ecc"),
            "inactive HBM4 section leaked into HBM3");

    selection = hbm_sim::config::discover_selection({lpddr}, "lpddr5", {});
    active = hbm_sim::config::resolve_document(lpddr, selection);
    require(has_entry(active, "standard.lpddr5", "lpddr_dvfs_mode", "disabled"),
            "LPDDR5 standard section was not activated");
    require(!has_entry(active, "standard.lpddr6", "lpddr_dvfs_mode"),
            "inactive LPDDR6 section leaked into LPDDR5");

    require(hbm_sim::config::parse_validation_mode("research") ==
                hbm_sim::config::ValidationMode::Exploratory,
            "research alias must select exploratory mode");
    require(hbm_sim::config::parse_validation_mode("device") ==
                hbm_sim::config::ValidationMode::Device,
            "device validation mode parse failed");

    require(hbm_sim::config::list_presets(hbm, "hbm4").empty() &&
                hbm_sim::config::list_presets(hbm, "hbm3").empty(),
            "HBM master must contain standard parameters only");
    const auto lpddr5_presets = hbm_sim::config::list_presets(lpddr, "lpddr5");
    const auto lpddr6_presets = hbm_sim::config::list_presets(lpddr, "lpddr6");
    require(lpddr5_presets.empty() && lpddr6_presets.size() == 2 &&
                std::find(lpddr6_presets.begin(), lpddr6_presets.end(),
                          "link_protection") != lpddr6_presets.end() &&
                std::find(lpddr6_presets.begin(), lpddr6_presets.end(),
                          "low_dvfs_4267") != lpddr6_presets.end(),
            "LPDDR master may contain only its two standard mode variants");

    const auto hbm3_validation = hbm_sim::config::load_document_tree(
        root + "/configs/validation/hbm3.cfg");
    require(hbm3_validation.size() == 2 &&
                hbm3_validation.front().path.find("configs/hbm.cfg") !=
                    std::string::npos,
            "validation inheritance must load the HBM master first");
    const auto validation_presets =
        hbm_sim::config::list_presets(hbm3_validation.back(), "hbm3");
    require(validation_presets.size() == 2 &&
                std::find(validation_presets.begin(), validation_presets.end(),
                          "ramulator2_reference_1ch") !=
                    validation_presets.end() &&
                std::find(validation_presets.begin(), validation_presets.end(),
                          "dramsim3_hbm2_common") != validation_presets.end(),
            "HBM3 validation preset inventory is incomplete");

    const auto hbm_case = hbm_sim::config::load_document_tree(
        root + "/configs/usecases/hbm_nstacks.cfg");
    require(hbm_case.size() == 2 && hbm_case.back().path.find(
                                        "hbm_nstacks.cfg") != std::string::npos,
            "usecase inheritance did not expand in base-to-child order");

    // 继承环必须在加载阶段失败，不能递归到栈溢出，也不能只取其中一份配置。
    const auto cycle_dir = std::filesystem::temp_directory_path() /
                           "hbm_sim_config_inheritance_cycle";
    std::filesystem::remove_all(cycle_dir);
    std::filesystem::create_directories(cycle_dir);
    {
      std::ofstream a(cycle_dir / "a.cfg");
      std::ofstream b(cycle_dir / "b.cfg");
      a << "[meta]\nextends = b.cfg # test-only cycle\n";
      b << "[meta]\nextends = a.cfg # test-only cycle\n";
    }
    bool rejected_inheritance_cycle = false;
    try {
      (void)hbm_sim::config::load_document_tree((cycle_dir / "a.cfg").string());
    } catch (const std::runtime_error &error) {
      rejected_inheritance_cycle =
          std::string(error.what()).find("inheritance cycle") !=
          std::string::npos;
    }
    std::filesystem::remove_all(cycle_dir);
    require(rejected_inheritance_cycle,
            "config inheritance cycle must fail with a clear diagnostic");

    hbm_sim::config::ConfigDocument override_doc;
    override_doc.path = "override.cfg";
    override_doc.sectioned = true;
    override_doc.entries.push_back(
        {"ncl", "28", "override", override_doc.path, 2});
    hbm_sim::config::ConfigDocument later_common_doc;
    later_common_doc.path = "later-common.cfg";
    later_common_doc.sectioned = true;
    later_common_doc.entries.push_back(
        {"ncl", "30", "timing", later_common_doc.path, 2});
    const auto globally_layered = hbm_sim::config::resolve_documents(
        {override_doc, later_common_doc}, {"hbm4", "baseline"});
    require(globally_layered.size() == 2 && globally_layered[0].value == "30" &&
                globally_layered[0].layer == 10 &&
                globally_layered[1].value == "28" &&
                globally_layered[1].layer == 50,
            "multiple config documents must use global layer ordering");

    hbm_sim::config::ConfigDocument preset_order_doc;
    preset_order_doc.path = "preset-order.cfg";
    preset_order_doc.sectioned = true;
    preset_order_doc.entries.push_back(
        {"stack_count", "1", "system", preset_order_doc.path, 2});
    preset_order_doc.entries.push_back({"stack_count", "2",
                                        "preset.hbm4.multistack.system",
                                        preset_order_doc.path, 4});
    const auto preset_order = hbm_sim::config::resolve_document(
        preset_order_doc, {"hbm4", "multistack"});
    require(preset_order.size() == 2 && preset_order[0].layer == 10 &&
                preset_order[1].layer == 40 && preset_order[1].value == "2",
            "selected preset must override common defaults");

    hbm_sim::config::ConfigDocument alias_collision;
    alias_collision.path = "alias-collision.cfg";
    alias_collision.sectioned = true;
    alias_collision.entries.push_back(
        {"type", "fcfs", "controller.scheduler", alias_collision.path, 2});
    alias_collision.entries.push_back(
        {"scheduler", "frfcfs", "controller", alias_collision.path, 4});
    bool rejected_alias_collision = false;
    try {
      (void)hbm_sim::config::resolve_document(alias_collision,
                                              {"hbm4", "baseline"});
    } catch (const std::runtime_error &error) {
      rejected_alias_collision =
          std::string(error.what())
              .find("semantic duplicate key 'scheduler'") != std::string::npos;
    }
    require(rejected_alias_collision,
            "canonical aliases at the same layer must not silently override");
    std::cout << "config tests passed\n";
    return EXIT_SUCCESS;
  } catch (const std::exception &error) {
    std::cerr << "config test failure: " << error.what() << '\n';
    return EXIT_FAILURE;
  }
}
