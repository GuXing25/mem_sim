#include <cstdlib>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

#include "hbm_sim/controller/controller.hpp"
#include "hbm_sim/dram/spec.hpp"
#include "hbm_sim/phy/mem_phy.hpp"
#include "hbm_sim/validation/dfi.hpp"

namespace {

void require(bool condition, const std::string& message) {
  if (!condition) {
    std::cerr << "phy test failed: " << message << '\n';
    std::exit(1);
  }
}

hbm_sim::Request request(std::uint64_t id, hbm_sim::RequestType type, std::size_t bytes) {
  hbm_sim::Request result;
  result.id = id;
  result.address = 0x1000;
  result.type = type;
  result.transfer_bytes = bytes;
  result.decoded.channel = 0;
  result.decoded.pseudo_channel = 0;
  result.decoded.sid = 0;
  result.decoded.rank = 0;
  result.decoded.bank_group = 0;
  result.decoded.bank = 0;
  result.decoded.row = 1;
  result.decoded.column = 0;
  return result;
}

void test_adapters() {
  hbm_sim::DramSpec hbm = hbm_sim::make_spec("hbm4");
  hbm_sim::HbmPhyAdapter hbm_adapter;
  auto hbm_row = hbm_adapter.encode(hbm, hbm_sim::Command::ACT, hbm_sim::BusClass::Row);
  auto hbm_col = hbm_adapter.encode(hbm, hbm_sim::Command::RD, hbm_sim::BusClass::Column);
  require(hbm_row.row_path && hbm_row.ca_edges >= 1, "HBM adapter must encode row CA");
  require(hbm_col.column_path, "HBM adapter must encode column CA");

  hbm_sim::DramSpec lpddr = hbm_sim::make_spec("lpddr6");
  hbm_sim::LpddrPhyAdapter lpddr_adapter;
  auto act1 = lpddr_adapter.encode(lpddr, hbm_sim::Command::ACT1, hbm_sim::BusClass::Unified);
  auto wck = lpddr_adapter.encode(lpddr, hbm_sim::Command::WCKTRAIN, hbm_sim::BusClass::Unified);
  require(act1.split_activate && act1.ca_edges == 2, "LPDDR adapter must preserve split ACT");
  require(wck.wck_event, "LPDDR adapter must expose WCK events");
}

void test_lifecycle_fifo_and_dvfs() {
  hbm_sim::DramSpec spec = hbm_sim::make_spec("lpddr6");
  auto image = std::make_shared<hbm_sim::MemoryImage>(spec);
  hbm_sim::MemPhyOptions options;
  options.mode = hbm_sim::MemPhyMode::Behavioral;
  options.read_fifo_depth = 1;
  options.reset_cycles = 2;
  options.initialization_cycles = 2;
  options.training_cycles = 2;
  hbm_sim::MemPhy phy(spec, options, image);
  require(!phy.ready_for_data(), "PHY must gate data during reset/training");
  for (hbm_sim::Cycle cycle = 1; cycle <= 5; cycle++) phy.tick(cycle);
  require(!phy.ready_for_data(), "training t-1 must still reject data");
  phy.tick(6);
  require(phy.ready_for_data(), "auto training must reach ready state");

  auto rd = request(1, hbm_sim::RequestType::Read,
                    static_cast<std::size_t>(spec.transaction_bytes()));
  rd.controller_sequence = 1;
  phy.accept_command(rd, hbm_sim::Command::RD, hbm_sim::BusClass::Unified, 7);
  const auto ready = phy.submit_data(rd, hbm_sim::Command::RD, 7);
  require(!phy.can_accept_data(hbm_sim::Command::RD), "depth-1 read FIFO must backpressure");
  for (hbm_sim::Cycle cycle = 7; cycle <= ready; cycle++) phy.tick(cycle);
  require(phy.take_completion(rd.id, rd.controller_sequence).has_value(),
          "read completion must leave the PHY");

  phy.accept_command(rd, hbm_sim::Command::DVFS, hbm_sim::BusClass::Unified, ready + 1);
  require(!phy.ready_for_data(), "LPDDR DVFS must create WCK retraining debt");
  phy.accept_command(rd, hbm_sim::Command::WCKTRAIN, hbm_sim::BusClass::Unified, ready + 2);
  require(phy.ready_for_data(), "WCKTRAIN must restore ready state");

  bool rejected = false;
  try {
    hbm_sim::MemPhyOptions bad;
    bad.mode = hbm_sim::MemPhyMode::Behavioral;
    bad.read_fifo_depth = 0;
    hbm_sim::MemPhy invalid(spec, bad, image);
  } catch (const std::invalid_argument&) {
    rejected = true;
  }
  require(rejected, "zero-depth FIFO must be rejected");
}

void test_pending_write_ordering() {
  hbm_sim::DramSpec spec = hbm_sim::make_spec("hbm3");
  hbm_sim::ControllerOptions options;
  options.phy.mode = hbm_sim::MemPhyMode::Behavioral;
  options.retain_command_trace = true;
  hbm_sim::Controller controller(spec, options);
  const std::size_t bytes = static_cast<std::size_t>(spec.transaction_bytes());
  std::vector<std::uint8_t> payload(bytes, 0xa5);
  auto wr = request(10, hbm_sim::RequestType::Write, bytes);
  wr.payload = payload;
  wr.has_payload = true;
  require(controller.enqueue(wr), "pending-order write enqueue");
  bool issued_write = false;
  for (int i = 0; i < 10000 && !issued_write; i++) {
    controller.tick();
    for (const auto& command : controller.issued_commands()) {
      issued_write = issued_write || command.command == hbm_sim::Command::WR ||
                     command.command == hbm_sim::Command::WRA;
    }
  }
  require(issued_write, "write must enter PHY before ordering probe");
  auto rd = request(11, hbm_sim::RequestType::Read, bytes);
  rd.expected_payload = payload;
  rd.has_expected_payload = true;
  require(controller.enqueue(rd), "pending-order read enqueue");
  controller.run_until_done(controller.clock() + 200000);
  require(controller.stats().data_mismatches == 0,
          "read must not overtake an older write pending in PHY");
}

void test_end_to_end(const std::string& standard) {
  hbm_sim::DramSpec spec = hbm_sim::make_spec(standard);
  hbm_sim::ControllerOptions options;
  options.phy.mode = hbm_sim::MemPhyMode::Behavioral;
  options.phy.dfi_version = "6.0.1";
  options.phy.reset_cycles = 2;
  options.phy.initialization_cycles = 2;
  options.phy.training_cycles = 2;
  options.retain_command_trace = true;
  hbm_sim::Controller controller(spec, options);

  const std::size_t bytes = static_cast<std::size_t>(spec.transaction_bytes());
  std::vector<std::uint8_t> payload(bytes);
  for (std::size_t i = 0; i < bytes; i++) payload[i] = static_cast<std::uint8_t>(i ^ 0x5a);

  auto wr = request(1, hbm_sim::RequestType::Write, bytes);
  wr.payload = payload;
  wr.has_payload = true;
  require(controller.enqueue(wr), standard + " write enqueue");
  controller.run_until_done(200000);

  auto rd = request(2, hbm_sim::RequestType::Read, bytes);
  rd.expected_payload = payload;
  rd.has_expected_payload = true;
  require(controller.enqueue(rd), standard + " read enqueue");
  controller.run_until_done(controller.clock() + 200000);

  const auto& stats = controller.stats();
  require(stats.completed_writes == 1 && stats.completed_reads == 1,
          standard + " requests must complete");
  require(stats.data_mismatches == 0, standard + " PHY data round trip must match");
  require(stats.phy_write_requests == 1 && stats.phy_read_requests == 1,
          standard + " data must traverse online PHY");
  require(stats.phy_write_completions == 1 && stats.phy_read_completions == 1,
          standard + " PHY completions must return to MC");
  if (spec.lpddr_family) {
    require(stats.phy_lpddr_split_act_events > 0, standard + " must use LPDDR adapter");
  } else {
    require(stats.phy_hbm_row_commands > 0 && stats.phy_hbm_column_commands > 0,
            standard + " must use HBM row/column adapter");
  }
  auto events = hbm_sim::build_dfi_trace(spec, controller.issued_commands());
  auto report = hbm_sim::validate_dfi_trace(spec, controller.issued_commands(), events);
  require(report.ok(), standard + " online completion trace must validate");
}

}  // namespace

int main() {
  test_adapters();
  test_lifecycle_fifo_and_dvfs();
  test_pending_write_ordering();
  for (const char* standard : {"hbm3", "hbm4", "lpddr5", "lpddr6"}) {
    test_end_to_end(standard);
  }
  std::cout << "phy tests passed\n";
  return 0;
}
