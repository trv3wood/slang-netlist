#include "slang/driver/Driver.h"

#include "common/Utilities.hpp"
#include "common/Wildcard.hpp"
#include "netlist/BuilderOptions.hpp"
#include "netlist/CombLoops.hpp"
#include "netlist/Debug.hpp"
#include "netlist/NetlistDiagnostics.hpp"
#include "netlist/NetlistDot.hpp"
#include "netlist/NetlistGraph.hpp"
#include "netlist/NetlistSerializer.hpp"
#include "netlist/PathFinder.hpp"
#include "netlist/VisitAll.hpp"

#include "common/FormatBuffer.hpp"
#include "slang/analysis/AnalysisManager.h"
#include "slang/ast/Compilation.h"
#include "slang/diagnostics/Diagnostics.h"
#include "slang/numeric/ConstantValue.h"
#include "slang/text/Json.h"
#include "slang/util/Util.h"
#include "slang/util/VersionInfo.h"

#include "fmt/color.h"
#include "fmt/format.h"
#include <algorithm>
#include <cctype>
#include <charconv>
#include <chrono>
#include <filesystem>
#include <iostream>
#include <nlohmann/json.hpp>
#include <string>
#include <unordered_set>
#include <utility>
#include <vector>

using namespace slang;
using namespace slang::ast;
using namespace slang::driver;
using namespace slang::netlist;

namespace {

/// Get the TextLocation for a node, if it has one.
auto getNodeLocation(NetlistNode const &node) -> std::optional<TextLocation> {
  switch (node.kind) {
  case NodeKind::Port:
    return node.as<Port>().location;
  case NodeKind::Assignment:
    return node.as<Assignment>().location;
  case NodeKind::Conditional:
    return node.as<Conditional>().location;
  case NodeKind::Case:
    return node.as<Case>().location;
  default:
    return std::nullopt;
  }
}

void reportNodeDiag(NetlistDiagnostics &diagnostics, NetlistNode const &node) {
  switch (node.kind) {
  case NodeKind::Port: {
    auto const &port = node.as<Port>();
    auto srcLoc = port.location.sourceLocation;
    if (port.isInput()) {
      Diagnostic diagnostic(diag::InputPort, srcLoc);
      diagnostic << port.name;
      diagnostics.issue(diagnostic);
    } else if (port.isOutput()) {
      Diagnostic diagnostic(diag::OutputPort, srcLoc);
      diagnostic << port.name;
      diagnostics.issue(diagnostic);
    } else {
      SLANG_UNREACHABLE;
    }
    break;
  }
  case NodeKind::Assignment: {
    auto const &assignment = node.as<Assignment>();
    Diagnostic diagnostic(diag::Assignment, assignment.location.sourceLocation);
    diagnostics.issue(diagnostic);
    break;
  }
  case NodeKind::Conditional: {
    auto const &conditional = node.as<Conditional>();
    Diagnostic diagnostic(diag::Conditional,
                          conditional.location.sourceLocation);
    diagnostics.issue(diagnostic);
    break;
  }
  case NodeKind::Case: {
    auto const &caseNode = node.as<Case>();
    Diagnostic diagnostic(diag::Case, caseNode.location.sourceLocation);
    diagnostics.issue(diagnostic);
    break;
  }
  case NodeKind::Merge:
    break;
  default:
    break;
  }
}

void reportEdgeDiag(NetlistDiagnostics &diagnostics, NetlistEdge &edge) {
  if (edge.symbol != nullptr && !edge.symbol->empty()) {
    Diagnostic diagnostic(diag::Value, edge.symbol->location.sourceLocation);
    diagnostic << fmt::format("{}{}", edge.symbol->hierarchicalPath,
                              toString(edge.bounds));
    diagnostics.issue(diagnostic);
  }
}

void reportNodeText(netlist::FormatBuffer &buffer, FileTable const &fileTable,
                    NetlistNode const &node) {
  switch (node.kind) {
  case NodeKind::Port: {
    auto const &port = node.as<Port>();
    auto loc = port.location.toString(fileTable);
    if (port.isInput()) {
      buffer.format("{}: note: input port {}\n", loc, port.name);
    } else if (port.isOutput()) {
      buffer.format("{}: note: output port {}\n", loc, port.name);
    } else {
      SLANG_UNREACHABLE;
    }
    break;
  }
  case NodeKind::Assignment: {
    auto const &assignment = node.as<Assignment>();
    buffer.format("{}: note: assignment\n",
                  assignment.location.toString(fileTable));
    break;
  }
  case NodeKind::Conditional: {
    auto const &conditional = node.as<Conditional>();
    buffer.format("{}: note: conditional statement\n",
                  conditional.location.toString(fileTable));
    break;
  }
  case NodeKind::Case: {
    auto const &caseNode = node.as<Case>();
    buffer.format("{}: note: case statement\n",
                  caseNode.location.toString(fileTable));
    break;
  }
  case NodeKind::Merge:
    break;
  default:
    break;
  }
}

void reportEdgeText(netlist::FormatBuffer &buffer, FileTable const &fileTable,
                    NetlistEdge &edge) {
  if (edge.symbol != nullptr && !edge.symbol->empty()) {
    buffer.format("{}: note: value {}{}\n",
                  edge.symbol->location.toString(fileTable),
                  edge.symbol->hierarchicalPath, toString(edge.bounds));
  }
}

/// Report a path using diagnostics (with source lines and carets)
/// when source locations are available, otherwise fall back to
/// plain text output.
auto reportPath(FileTable const &fileTable, NetlistDiagnostics *diagnostics,
                const NetlistPath &path) -> std::string {

  // Check if the first located node has a source location to
  // decide which reporting mode to use.
  bool useDiag = false;
  if (diagnostics) {
    for (auto const *node : path) {
      if (auto loc = getNodeLocation(*node)) {
        useDiag = loc->hasSourceLocation();
        break;
      }
    }
  }

  if (useDiag) {
    for (size_t i = 0; i < path.size() - 1; ++i) {
      auto const *nodeA = path[i];
      auto const *nodeB = path[i + 1];
      auto edgeIt = nodeA->findEdgeTo(*nodeB);
      SLANG_ASSERT(edgeIt != nodeA->end() &&
                   "edge between nodes not found in path");
      reportNodeDiag(*diagnostics, *nodeA);
      reportEdgeDiag(*diagnostics, **edgeIt);
    }
    reportNodeDiag(*diagnostics, *path.back());
    auto result = diagnostics->getString();
    diagnostics->clear();
    return std::string(result);
  }

  netlist::FormatBuffer buffer;
  for (size_t i = 0; i < path.size() - 1; ++i) {
    auto const *nodeA = path[i];
    auto const *nodeB = path[i + 1];
    auto edgeIt = nodeA->findEdgeTo(*nodeB);
    SLANG_ASSERT(edgeIt != nodeA->end() &&
                 "edge between nodes not found in path");
    reportNodeText(buffer, fileTable, *nodeA);
    reportEdgeText(buffer, fileTable, **edgeIt);
  }
  reportNodeText(buffer, fileTable, *path.back());
  return buffer.str();
}

}; // namespace

auto main(int argc, char **argv) -> int {
  OS::setupConsole();

  // Skill 导出是独立操作，不需要解析或 elaboration RTL。
  if (argc == 4 && std::string_view(argv[1]) == "skill" &&
      std::string_view(argv[2]) == "export") {
    namespace fs = std::filesystem;
    fs::path destination(argv[3]);
    if (fs::exists(destination)) {
      fmt::print(stderr, "error: skill destination already exists: {}\n",
                 destination.string());
      return 2;
    }
    fs::path source = SLANG_NETLIST_SKILL_SOURCE;
    auto installed =
        fs::weakly_canonical(fs::path(argv[0])).parent_path().parent_path() /
        "share/slang-netlist/skill";
    if (fs::exists(installed)) {
      source = installed;
    }
    std::error_code error;
    fs::copy(source, destination, fs::copy_options::recursive, error);
    if (error) {
      fmt::print(stderr, "error: could not export skill: {}\n",
                 error.message());
      return 1;
    }
    fmt::print("Exported slang-netlist {} skill to {}\n", SLANG_NETLIST_VERSION,
               destination.string());
    return 0;
  }

  Driver driver;
  driver.addStandardArgs();

  std::optional<bool> showHelp;
  driver.cmdLine.add("-h,--help", showHelp, "Display available options");

  std::optional<bool> showVersion;
  driver.cmdLine.add("--version", showVersion,
                     "Display version information and exit");

  std::optional<bool> noColours;
  driver.cmdLine.add("--no-colours", noColours,
                     "Disable colored output (default is enabled on terminals "
                     "that support it)");

  std::optional<bool> quiet;
  driver.cmdLine.add("-q,--quiet", quiet, "Suppress non-essential output");

  std::optional<bool> stats;
  driver.cmdLine.add("--stats", stats,
                     "Print execution statistics (phase timings and peak "
                     "memory) to stderr");

  std::optional<bool> statsJson;
  driver.cmdLine.add("--stats-json", statsJson,
                     "Print execution statistics as JSON to stdout");

  std::optional<bool> debug;
  driver.cmdLine.add("-d,--debug", debug, "Output debugging information");

  std::optional<bool> reportRegisters;
  driver.cmdLine.add("--report-registers", reportRegisters,
                     "Report all registers in the design to stdout");

  std::optional<bool> combLoops;
  driver.cmdLine.add("--comb-loops", combLoops,
                     "Report any combinational loops in the design to stdout");

  std::optional<bool> noResolveAssignBits;
  driver.cmdLine.add(
      "--no-resolve-assign-bits", noResolveAssignBits,
      "Disable bit-aligned dependency resolution of concatenations, "
      "replications, conversions, and equal-width conditional operators "
      "in assignments and port connections. When set, each LSP on one "
      "side of an assignment fans into every LSP on the other side.");

  std::optional<bool> noPropCutsAcrossPorts;
  driver.cmdLine.add(
      "--no-prop-cuts-across-ports", noPropCutsAcrossPorts,
      "Disable propagation of concat-induced cut points across module "
      "port boundaries. When set, port nodes and module-internal "
      "assignments stay whole-word at port boundaries; "
      "scalar->concat->port->concat->scalar paths are bit-imprecise.");

  std::vector<std::string> blackBoxes;
  driver.cmdLine.add(
      "--black-box", blackBoxes,
      "Glob pattern matched against each instance's definition name and "
      "hierarchical path; matched instances record only port-boundary "
      "connectivity. May be repeated. Supports `*` (within a path "
      "segment), `**` or `...` (recursive across `.`), and `?` (single "
      "char within a segment).",
      "<pattern>");

  std::optional<std::string> netlistDotFile;
  driver.cmdLine.add("--netlist-dot", netlistDotFile,
                     "Dump the netlist in DOT format to the specified file, "
                     "or '-' for stdout. Combine with --fan-out, --fan-in or "
                     "--from/--to to render only that cone or path.",
                     "<file>", CommandLineFlags::FilePath);

  std::optional<std::string> fromPointName;
  driver.cmdLine.add("--from", fromPointName,
                     "Specify a start point from which to trace a path. Used "
                     "alone (without --to), reports the combinational fan-out "
                     "cone from the node.",
                     "<name>");

  std::optional<std::string> toPointName;
  driver.cmdLine.add("--to", toPointName,
                     "Specify a finish point to trace a path to. Used alone "
                     "(without --from), reports the combinational fan-in cone "
                     "to the node.",
                     "<name>");

  std::optional<uint64_t> fromNodeId;
  driver.cmdLine.add("--from-node-id", fromNodeId,
                     "Select the path start by artifact-scoped node ID",
                     "<id>");
  std::optional<uint64_t> toNodeId;
  driver.cmdLine.add("--to-node-id", toNodeId,
                     "Select the path finish by artifact-scoped node ID",
                     "<id>");
  std::optional<std::string> fromKind;
  driver.cmdLine.add("--from-kind", fromKind,
                     "Disambiguate the named path start by node kind",
                     "<kind>");
  std::optional<std::string> toKind;
  driver.cmdLine.add("--to-kind", toKind,
                     "Disambiguate the named path finish by node kind",
                     "<kind>");

  std::optional<std::string> fanOutName;
  driver.cmdLine.add("--fan-out", fanOutName,
                     "Report the combinational fan-out cone from a named node",
                     "<name>");

  std::optional<std::string> fanInName;
  driver.cmdLine.add("--fan-in", fanInName,
                     "Report the combinational fan-in cone to a named node",
                     "<name>");

  std::optional<std::string> sensitivityName;
  driver.cmdLine.add(
      "--sensitivity", sensitivityName,
      "Report the clocks/resets gating a named node. For a register the "
      "node's own clocking edges are listed; otherwise the union over every "
      "register in its combinational fan-out.",
      "<name>");

  std::optional<std::string> constantDriversName;
  driver.cmdLine.add(
      "--constant-drivers", constantDriversName,
      "Report the constant values driving a named node when its "
      "combinational fan-in bottoms out only at literals (i.e. the node is "
      "tied off). Prints nothing driving it if any register or external "
      "input reaches the node.",
      "<name>");

  std::optional<std::string> driversName;
  driver.cmdLine.add(
      "--drivers", driversName,
      "Report, per bit, which node drives a named signal. An optional bit "
      "range narrows the query, e.g. `m.sig[3:0]` or `m.sig[2]`; without one "
      "the whole signal is reported.",
      "<name[bit-range]>");

  std::optional<std::string> findPattern;
  driver.cmdLine.add(
      "--find", findPattern,
      "Find named nodes matching a glob pattern. Supports `*` (within a "
      "path segment), `**` or `...` (recursive across `.`), and `?` "
      "(single char within a segment).",
      "<pattern>");

  std::optional<std::string> findRegexPattern;
  driver.cmdLine.add("--find-regex", findRegexPattern,
                     "Find named nodes matching a regex pattern", "<pattern>");

  std::optional<std::string> format;
  driver.cmdLine.add(
      "--format", format,
      "Output format for the tabular query commands (--report-registers, "
      "--find, --find-regex, --fan-out, --fan-in, --sensitivity, "
      "--constant-drivers, --drivers): 'table' (default) or 'json'",
      "<table|json>");

  std::vector<std::string> scopeFilters;
  driver.cmdLine.add(
      "--scope", scopeFilters,
      "Restrict the node-listing query commands (--report-registers, --find, "
      "--find-regex, --fan-out, --fan-in, --sensitivity) to a hierarchical "
      "subtree. A node passes if its path is the scope or a descendant of it "
      "(so `top.cpu` matches `top.cpu.alu.x` but not `top.cpu2`). Literal "
      "paths only; use --name for globs. May be repeated.",
      "<path>");

  std::vector<std::string> nameFilters;
  driver.cmdLine.add(
      "--name", nameFilters,
      "Restrict the same node-listing query commands to nodes whose "
      "hierarchical path matches a glob pattern (same syntax as --find: "
      "`*`/`?` stay within a segment, `**`/`...` cross boundaries). May be "
      "repeated.",
      "<pattern>");

  std::optional<std::string> outputFile;
  driver.cmdLine.add("-o,--output", outputFile,
                     "Write tabular query output to the given file instead of "
                     "stdout ('-' for stdout)",
                     "<file>", CommandLineFlags::FilePath);

  std::optional<uint64_t> maxResults;
  driver.cmdLine.add("--max-results", maxResults,
                     "Maximum number of machine-query results (default 200; "
                     "0 means unlimited)",
                     "<count>");

  std::optional<uint64_t> maxDepth;
  driver.cmdLine.add("--max-depth", maxDepth,
                     "Maximum traversal depth (default 64; 0 means unlimited)",
                     "<depth>");

  std::optional<std::string> crossState;
  driver.cmdLine.add("--cross-state", crossState,
                     "State traversal policy for paths: never (default), "
                     "once, or unlimited",
                     "<never|once|unlimited>");

  std::optional<std::string> saveNetlistFile;
  driver.cmdLine.add("--save-netlist", saveNetlistFile,
                     "Save the netlist to a JSON file", "<file>",
                     CommandLineFlags::FilePath);

  std::optional<std::string> loadNetlistFile;
  driver.cmdLine.add("--load-netlist", loadNetlistFile,
                     "Load a netlist from a JSON file (skips compilation)",
                     "<file>", CommandLineFlags::FilePath);

  if (!driver.parseCommandLine(argc, argv)) {
    return 1;
  }

  if (showHelp == true) {
    std::cout << fmt::format(
        "{}\n",
        driver.cmdLine.getHelpText("slang SystemVerilog netlist tool").c_str());
    return 0;
  }

  if (showVersion == true) {
    printf("slang-netlist version %s (slang %d.%d.%d+%s)\n",
           SLANG_NETLIST_VERSION, VersionInfo::getMajor(),
           VersionInfo::getMinor(), VersionInfo::getPatch(),
           std::string(VersionInfo::getHash()).c_str());
    return 0;
  }

  if (debug) {
    Config::getInstance().debugEnabled = true;
  }

  if (quiet) {
    Config::getInstance().quietEnabled = true;
  }

  // Output format for the tabular query commands.
  enum class Format { Table, Json };
  auto outputFormat = Format::Table;
  if (format) {
    if (*format == "json") {
      outputFormat = Format::Json;
    } else if (*format == "table") {
      outputFormat = Format::Table;
    } else {
      fmt::print(stderr,
                 "error: unknown --format value '{}'; expected 'table' or "
                 "'json'\n",
                 *format);
      return 1;
    }
  }
  if (statsJson) {
    outputFormat = Format::Json;
  }
  auto crossStatePolicy = crossState.value_or("never");
  if (crossStatePolicy != "never" && crossStatePolicy != "once" &&
      crossStatePolicy != "unlimited") {
    fmt::print(stderr, "error: invalid --cross-state value '{}'\n",
               crossStatePolicy);
    return 2;
  }

  using Json = nlohmann::json;
  std::optional<Json> pendingEnvelope;
  int machineExitCode = 0;
  std::optional<size_t> queryTotal;

  auto writeOutput = [&](std::string_view content) {
    if (outputFile && *outputFile != "-") {
      OS::writeFile(*outputFile, content);
    } else {
      OS::print(content);
    }
  };

  // Emit a table either as a formatted text table or as a JSON array of
  // objects keyed by the lower-cased column headers.
  auto emitTable = [&](std::string_view command, Utilities::Row const &header,
                       Utilities::Table const &table) {
    if (outputFormat == Format::Json) {
      Json items = Json::array();
      auto limit = maxResults.value_or(200);
      auto count =
          limit == 0 ? table.size() : std::min(table.size(), size_t(limit));
      for (size_t rowIndex = 0; rowIndex < count; ++rowIndex) {
        auto const &row = table[rowIndex];
        Json item = Json::object();
        for (size_t i = 0; i < header.size() && i < row.size(); ++i) {
          std::string key(header[i].size(), '\0');
          std::transform(header[i].begin(), header[i].end(), key.begin(),
                         [](unsigned char c) { return std::tolower(c); });
          item[key] = row[i];
        }
        items.push_back(std::move(item));
      }
      auto total = queryTotal.value_or(table.size());
      auto truncated = count < table.size() || total > table.size();
      machineExitCode = truncated ? 4 : (table.empty() ? 3 : 0);
      pendingEnvelope =
          Json{{"schema_version", 1},
               {"tool",
                {{"name", "slang-netlist"},
                 {"version", SLANG_NETLIST_VERSION},
                 {"slang_version",
                  fmt::format("{}.{}.{}+{}", VersionInfo::getMajor(),
                              VersionInfo::getMinor(), VersionInfo::getPatch(),
                              VersionInfo::getHash())},
                 {"graph_schema_version", NetlistSerializer::formatVersion}}},
               {"command", command},
               {"query", Json::object()},
               {"data", {{"items", std::move(items)}}},
               {"diagnostics", Json::array()},
               {"summary",
                {{"status",
                  truncated ? "truncated" : (table.empty() ? "empty" : "ok")},
                 {"complete", !truncated},
                 {"returned", count},
                 {"total", total}}}};
      queryTotal.reset();
    } else {
      netlist::FormatBuffer buffer;
      Utilities::formatTable(buffer, header, table);
      writeOutput(buffer.str());
    }
  };

  // Whether a node's hierarchical path passes the --scope/--name filters. A
  // node must fall within some --scope subtree (if any given) and match some
  // --name glob (if any given). With no filters set, everything passes.
  auto passesFilters = [&](std::string_view path) -> bool {
    if (!scopeFilters.empty()) {
      auto inScope = std::any_of(
          scopeFilters.begin(), scopeFilters.end(),
          [&](auto const &scope) { return pathInScope(path, scope); });
      if (!inScope) {
        return false;
      }
    }
    if (!nameFilters.empty()) {
      std::string subject(path);
      auto named = std::any_of(
          nameFilters.begin(), nameFilters.end(), [&](auto const &p) {
            return wildcardMatch(subject.c_str(), p.c_str());
          });
      if (!named) {
        return false;
      }
    }
    return true;
  };

  // Split a query of the form "path[hi:lo]" or "path[bit]" into a base path
  // and an optional bit range. A trailing bracketed expression that does not
  // parse as a range is treated as part of the name.
  auto parseNameAndRange = [](std::string_view spec)
      -> std::pair<std::string, std::optional<DriverBitRange>> {
    auto parseInt = [](std::string_view s) -> std::optional<int32_t> {
      int32_t value = 0;
      auto *end = s.data() + s.size();
      auto [ptr, ec] = std::from_chars(s.data(), end, value);
      if (ec != std::errc{} || ptr != end) {
        return std::nullopt;
      }
      return value;
    };
    auto whole = std::string(spec);
    if (spec.empty() || spec.back() != ']') {
      return {whole, std::nullopt};
    }
    auto open = spec.rfind('[');
    if (open == std::string_view::npos) {
      return {whole, std::nullopt};
    }
    auto inner = spec.substr(open + 1, spec.size() - open - 2);
    auto path = std::string(spec.substr(0, open));
    auto colon = inner.find(':');
    if (colon == std::string_view::npos) {
      auto bit = parseInt(inner);
      if (!bit) {
        return {whole, std::nullopt};
      }
      return {path, DriverBitRange{*bit, *bit}};
    }
    auto hi = parseInt(inner.substr(0, colon));
    auto lo = parseInt(inner.substr(colon + 1));
    if (!hi || !lo) {
      return {whole, std::nullopt};
    }
    return {path, DriverBitRange{*hi, *lo}};
  };

  // Human-readable description of a driver node for the --drivers report.
  auto describeDriver = [](NetlistNode &node) -> std::string {
    switch (node.kind) {
    case NodeKind::Port: {
      auto const &port = node.as<Port>();
      auto const *dir =
          port.isInput() ? "input" : (port.isOutput() ? "output" : "inout");
      return fmt::format("{} port {}", dir, port.hierarchicalPath);
    }
    case NodeKind::Variable:
      return fmt::format("variable {}", node.as<Variable>().hierarchicalPath);
    case NodeKind::State:
      return fmt::format("register {}", node.as<State>().hierarchicalPath);
    case NodeKind::Constant:
      return fmt::format("constant {}", node.as<Constant>().value.toString());
    case NodeKind::Assignment:
      return "assignment";
    case NodeKind::Conditional:
      return "conditional";
    case NodeKind::Case:
      return "case";
    case NodeKind::Merge:
      return "merge";
    default:
      return "unknown";
    }
  };

  using Clock = std::chrono::steady_clock;
  std::vector<std::pair<std::string, double>> phaseTimes;

  auto timePhase = [&](const std::string &name, auto &&fn) {
    auto start = Clock::now();
    fn();
    std::chrono::duration<double> elapsed = Clock::now() - start;
    phaseTimes.emplace_back(name, elapsed.count());
  };

  // Pointer to the graph, set once it's constructed, for printStats access.
  NetlistGraph *graphPtr = nullptr;

  auto makeStatsJson = [&] {
    auto peakRSS = OS::getPeakMemoryBytes();
    Json result;
    for (auto &[name, seconds] : phaseTimes) {
      result["time_seconds"][name] = seconds;
    }
    result["peak_rss_bytes"] = peakRSS;

    if (graphPtr) {
      auto const &bp = graphPtr->getBuildProfile();
      result["netlist_profile"] = {
          {"phase1_collect_seconds", bp.phase1_collectSeconds},
          {"phase2_parallel_seconds", bp.phase2_parallelSeconds},
          {"phase3_drain_seconds", bp.phase3_drainSeconds},
          {"phase4_rvalue_seconds", bp.phase4_rvalueSeconds},
          {"drain_pending_rvalues_seconds", bp.drain_pendingRValuesSeconds},
          {"drain_merges_seconds", bp.drain_mergesSeconds},
          {"deferred_block_count", bp.deferredBlockCount},
          {"deferred_pending_rvalue_count", bp.deferredPendingRValueCount},
          {"task_min_seconds", bp.taskMinSeconds},
          {"task_max_seconds", bp.taskMaxSeconds},
          {"task_mean_seconds", bp.taskMeanSeconds},
          {"task_median_seconds", bp.taskMedianSeconds},
          {"task_total_seconds", bp.taskTotalSeconds},
          {"num_threads", bp.numThreads}};
    }
    return result;
  };

  auto printStatsHuman = [&] {
    auto peakRSS = OS::getPeakMemoryBytes();
    double total = 0;
    for (auto &[name, seconds] : phaseTimes) {
      total += seconds;
    }

    auto fmtTime = [](double s) { return fmt::format("{:.3f}s", s); };

    netlist::FormatBuffer buf;

    buf.format("\nPhase Timing\n");
    Utilities::Table phaseRows;
    for (auto &[name, seconds] : phaseTimes) {
      phaseRows.push_back({name, fmtTime(seconds)});
    }
    phaseRows.push_back({"total", fmtTime(total)});
    Utilities::formatTable(buf, {"Phase", "Time"}, phaseRows);

    if (graphPtr && graphPtr->getBuildProfile().deferredBlockCount > 0) {
      auto const &bp = graphPtr->getBuildProfile();

      buf.format("\nNetlist Build ({} thread{})\n", bp.numThreads,
                 bp.numThreads == 1 ? "" : "s");
      Utilities::formatTable(
          buf, {"Phase", "Time"},
          {{"collect", fmtTime(bp.phase1_collectSeconds)},
           {"parallel DFA", fmtTime(bp.phase2_parallelSeconds)},
           {"drain", fmtTime(bp.phase3_drainSeconds)},
           {"resolve R-values", fmtTime(bp.phase4_rvalueSeconds)},
           {"total", fmtTime(bp.totalSeconds())}});

      if (bp.deferredBlockCount > 0) {
        buf.format("\nDFA Tasks ({} blocks, {} pending R-values)\n",
                   bp.deferredBlockCount, bp.deferredPendingRValueCount);
        Utilities::formatTable(buf, {"Statistic", "Time"},
                               {{"min", fmtTime(bp.taskMinSeconds)},
                                {"max", fmtTime(bp.taskMaxSeconds)},
                                {"mean", fmtTime(bp.taskMeanSeconds)},
                                {"median", fmtTime(bp.taskMedianSeconds)}});
      }
    }

    buf.format("\nPeak RSS: {:.1f} MB\n",
               static_cast<double>(peakRSS) / (1024.0 * 1024.0));
    fmt::print(stderr, "{}", buf.str());
  };

  auto printStats = [&] {
    if (stats && outputFormat == Format::Table) {
      printStatsHuman();
    }
    if (pendingEnvelope) {
      if (graphPtr) {
        (*pendingEnvelope)["artifact_id"] = graphPtr->getArtifactId();
      }
      if (stats || statsJson) {
        (*pendingEnvelope)["stats"] = makeStatsJson();
      }
      writeOutput(pendingEnvelope->dump(2) + "\n");
      pendingEnvelope.reset();
    }
  };

  SLANG_TRY {

    NetlistGraph graph;
    graphPtr = &graph;
    std::unique_ptr<Compilation> compilation;
    std::unique_ptr<NetlistDiagnostics> diagnostics;

    if (loadNetlistFile) {
      // Load a previously-saved netlist (skips compilation).
      SmallVector<char> fileContent;
      auto ec = OS::readFile(*loadNetlistFile, fileContent);
      if (ec) {
        SLANG_THROW(std::runtime_error(
            fmt::format("could not read file: {}", *loadNetlistFile)));
      }
      NetlistSerializer::deserialize(
          std::string_view(fileContent.data(), fileContent.size()), graph);

      DEBUG_PRINT("Loaded netlist has {} nodes and {} edges\n",
                  graph.numNodes(), graph.numEdges());
    } else {
      // Build a netlist from source files.
      if (!driver.processOptions()) {
        return 2;
      }

      bool parseOk = false;
      timePhase("parsing", [&] { parseOk = driver.parseAllSources(); });
      if (!parseOk) {
        return 1;
      }

      timePhase("elaboration", [&] {
        compilation = driver.createCompilation();
        driver.reportCompilation(*compilation, true);

        // Force construction of the whole AST.
        VisitAll va;
        compilation->getRoot().visit(va);

        // Freeze the compilation for subsequent multithreaded analysis.
        compilation->freeze();
      });

      if (!driver.reportDiagnostics(true)) {
        return 1;
      }

      std::unique_ptr<analysis::AnalysisManager> analysisManager;
      timePhase("analysis",
                [&] { analysisManager = driver.runAnalysis(*compilation); });
      if (!driver.reportDiagnostics(true)) {
        return 1;
      }

      timePhase("netlist", [&] {
        BuilderOptions const opts{
            .resolveAssignBits = !noResolveAssignBits.value_or(false),
            .propCutsAcrossPorts = !noPropCutsAcrossPorts.value_or(false),
            .numThreads = driver.options.numThreads.value_or(0),
            .blackBoxes = blackBoxes};
        graph.build(*compilation, *analysisManager, opts);
      });

      DEBUG_PRINT("Netlist has {} nodes and {} edges\n", graph.numNodes(),
                  graph.numEdges());

      if (saveNetlistFile) {
        auto json = NetlistSerializer::serialize(graph);
        OS::writeFile(*saveNetlistFile, json);
        printStats();
        return 0;
      }

      diagnostics =
          std::make_unique<NetlistDiagnostics>(*compilation, !noColours);
    }

    // --- Analysis commands that work on both built and loaded netlists ---

    auto locationJson = [&](std::optional<TextLocation> const &location) {
      if (!location) {
        return Json(nullptr);
      }
      return Json{{"file", std::string(graph.fileTable.getFilename(
                               location->fileIndex))},
                  {"line", location->line},
                  {"column", location->column}};
    };

    auto nodeJson = [&](NetlistNode const &node) {
      Json result{{"id", node.ID},
                  {"kind", toString(node.kind)},
                  {"path", node.getHierarchicalPath()
                               ? Json(*node.getHierarchicalPath())
                               : Json(nullptr)},
                  {"bounds", node.getBounds()
                                 ? Json::array({node.getBounds()->lower(),
                                                node.getBounds()->upper()})
                                 : Json(nullptr)},
                  {"location", locationJson(node.getLocation())}};
      result["name"] = node.getHierarchicalPath()
                           ? Json(*node.getHierarchicalPath())
                           : Json(nullptr);
      auto coverage = graph.getBlackBoxCoverage(node);
      result["black_box_coverage"] =
          coverage == BlackBoxCoverage::Boundary
              ? "boundary"
              : (coverage == BlackBoxCoverage::Contained ? "contained"
                                                         : "outside");
      if (node.kind == NodeKind::Constant) {
        // 常量值属于节点语义，机器输出不能只暴露显示名称。
        result["value"] = node.as<Constant>().value.toString();
      }
      return result;
    };

    auto edgeJson = [&](NetlistEdge const &edge) {
      return Json{
          {"source", edge.getSourceNode().ID},
          {"target", edge.getTargetNode().ID},
          {"symbol",
           edge.symbol ? Json(edge.symbol->hierarchicalPath) : Json(nullptr)},
          {"bounds", Json::array({edge.bounds.lower(), edge.bounds.upper()})},
          {"edge_kind", ast::toString(edge.edgeKind)},
          {"role", toString(edge.role)},
          {"precision", toString(edge.precision)},
          {"disabled", edge.disabled}};
    };

    auto setEnvelope = [&](std::string_view command, Json query, Json data,
                           size_t returned, size_t total,
                           bool complete = true) {
      pendingEnvelope =
          Json{{"schema_version", 1},
               {"tool",
                {{"name", "slang-netlist"},
                 {"version", SLANG_NETLIST_VERSION},
                 {"slang_version",
                  fmt::format("{}.{}.{}+{}", VersionInfo::getMajor(),
                              VersionInfo::getMinor(), VersionInfo::getPatch(),
                              VersionInfo::getHash())},
                 {"graph_schema_version", NetlistSerializer::formatVersion}}},
               {"artifact_id", graph.getArtifactId()},
               {"command", command},
               {"query", std::move(query)},
               {"data", std::move(data)},
               {"diagnostics", Json::array()},
               {"summary",
                {{"status",
                  !complete ? "truncated" : (returned == 0 ? "empty" : "ok")},
                 {"complete", complete},
                 {"returned", returned},
                 {"total", total}}}};
    };

    auto emitNodeGraph = [&](std::string_view command, Json query,
                             std::vector<NetlistNode *> const &input,
                             std::optional<size_t> knownTotal = std::nullopt) {
      auto limit = maxResults.value_or(200);
      auto count =
          limit == 0 ? input.size() : std::min(input.size(), size_t(limit));
      std::unordered_set<NetlistNode const *> included;
      Json nodes = Json::array();
      for (size_t i = 0; i < count; ++i) {
        included.insert(input[i]);
        nodes.push_back(nodeJson(*input[i]));
      }
      Json edges = Json::array();
      for (auto const *node : included) {
        for (auto const &edge : node->getOutEdges()) {
          if (included.contains(&edge->getTargetNode())) {
            edges.push_back(edgeJson(*edge));
          }
        }
      }
      auto total = knownTotal.value_or(input.size());
      auto complete = count == input.size() && total == input.size();
      auto items = nodes;
      setEnvelope(command, std::move(query),
                  {{"nodes", std::move(nodes)},
                   {"edges", std::move(edges)},
                   {"items", std::move(items)}},
                  count, total, complete);
      printStats();
      return complete ? (count == 0 ? 3 : 0) : 4;
    };

    auto parseNodeKind = [](std::string_view value) -> std::optional<NodeKind> {
      if (value == "port")
        return NodeKind::Port;
      if (value == "variable")
        return NodeKind::Variable;
      if (value == "assignment")
        return NodeKind::Assignment;
      if (value == "conditional")
        return NodeKind::Conditional;
      if (value == "case")
        return NodeKind::Case;
      if (value == "merge")
        return NodeKind::Merge;
      if (value == "state")
        return NodeKind::State;
      if (value == "constant")
        return NodeKind::Constant;
      return std::nullopt;
    };

    auto resolvePathNode = [&](std::string const &name,
                               std::optional<uint64_t> id,
                               std::optional<std::string> const &kind,
                               std::string_view label) -> NetlistNode * {
      if (id) {
        auto *node = graph.lookupById(*id);
        if (node == nullptr) {
          SLANG_THROW(std::runtime_error(
              fmt::format("could not find {} node ID: {}", label, *id)));
        }
        return node;
      }
      auto candidates = graph.lookupAll(name);
      if (kind) {
        auto parsed = parseNodeKind(*kind);
        if (!parsed) {
          SLANG_THROW(std::runtime_error(
              fmt::format("invalid {} node kind: {}", label, *kind)));
        }
        std::erase_if(candidates,
                      [&](auto *node) { return node->kind != *parsed; });
      }
      if (candidates.empty()) {
        SLANG_THROW(std::runtime_error(
            fmt::format("could not find {} point: {}", label, name)));
      }
      if (candidates.size() > 1) {
        SLANG_THROW(std::runtime_error(
            fmt::format("ambiguous {} point: {}; use --{}-kind or --{}-node-id",
                        label, name, label, label)));
      }
      return candidates[0];
    };

    // A lone --from/--to endpoint means "the reachable cone", which is exactly
    // the combinational fan-out/fan-in from that node. Alias it onto the
    // corresponding cone selector so every downstream handler (tabular output
    // and scoped --netlist-dot) treats them uniformly. When both endpoints are
    // given, path-finding takes over instead; an explicit --fan-out/--fan-in
    // always wins.
    if (fromPointName && !toPointName && !fanOutName) {
      fanOutName = fromPointName;
    } else if (toPointName && !fromPointName && !fanInName) {
      fanInName = toPointName;
    }

    if (reportRegisters) {
      if (outputFormat == Format::Json) {
        std::vector<NetlistNode *> states;
        for (auto const &node : graph.filterNodes(NodeKind::State)) {
          auto const &state = node->as<State>();
          if (passesFilters(state.hierarchicalPath))
            states.push_back(node.get());
        }
        return emitNodeGraph("registers", Json::object(), states);
      }
      auto header = Utilities::Row{"Name", "Location"};
      auto table = Utilities::Table{};

      for (auto const &node : graph.filterNodes(NodeKind::State)) {
        auto const &stateNode = node->as<State>();
        if (!passesFilters(stateNode.hierarchicalPath)) {
          continue;
        }
        auto loc = stateNode.location.toString(graph.fileTable);
        table.push_back(Utilities::Row{stateNode.hierarchicalPath, loc});
      }

      emitTable("registers", header, table);
      printStats();
      return outputFormat == Format::Json ? machineExitCode : 0;
    }

    // Report combinational loops.
    if (combLoops) {
      CombLoops combLoopsAnalysis(graph);
      auto cycles = combLoopsAnalysis.getAllLoops();
      if (outputFormat == Format::Json) {
        Json loops = Json::array();
        auto limit = maxResults.value_or(200);
        auto count =
            limit == 0 ? cycles.size() : std::min(cycles.size(), size_t(limit));
        for (size_t cycleIndex = 0; cycleIndex < count; ++cycleIndex) {
          Json nodes = Json::array();
          Json edges = Json::array();
          auto const &cycle = cycles[cycleIndex];
          for (size_t i = 0; i < cycle.size(); ++i) {
            nodes.push_back(nodeJson(*cycle[i]));
            if (i + 1 < cycle.size()) {
              auto edge = cycle[i]->findEdgeTo(*cycle[i + 1]);
              if (edge != cycle[i]->end())
                edges.push_back(edgeJson(**edge));
            }
          }
          loops.push_back(
              {{"nodes", std::move(nodes)}, {"edges", std::move(edges)}});
        }
        setEnvelope("comb-loops", Json::object(), {{"loops", std::move(loops)}},
                    count, cycles.size(), count == cycles.size());
        printStats();
        return cycles.empty() ? 3 : (count == cycles.size() ? 0 : 4);
      }
      if (cycles.empty()) {
        OS::print("No combinational loops detected in the design.\n");
      } else {
        for (auto const &cycle : cycles) {
          OS::print("Combinational loop detected:\n\n");
          auto result = reportPath(graph.fileTable, diagnostics.get(), cycle);
          OS::print(fmt::format("{}\n", result));
        }
      }
      printStats();
      return 0;
    }

    // Output a DOT file of the netlist. When combined with a fan-out, fan-in
    // or path selector, render only that induced subgraph instead of the
    // whole netlist.
    if (netlistDotFile) {
      auto requireNode = [&](std::string const &name) -> NetlistNode * {
        auto *node = graph.lookup(name);
        if (node == nullptr) {
          SLANG_THROW(
              std::runtime_error(fmt::format("could not find node: {}", name)));
        }
        return node;
      };

      netlist::FormatBuffer buffer;
      if (fanOutName || fanInName || (fromPointName && toPointName)) {
        std::unordered_set<NetlistNode const *> scope;
        if (fanOutName) {
          auto *node = requireNode(*fanOutName);
          scope.insert(node);
          for (auto *n : graph.getCombFanOut(*node)) {
            scope.insert(n);
          }
        } else if (fanInName) {
          auto *node = requireNode(*fanInName);
          scope.insert(node);
          for (auto *n : graph.getCombFanIn(*node)) {
            scope.insert(n);
          }
        } else {
          auto *fromPoint = requireNode(*fromPointName);
          auto *toPoint = requireNode(*toPointName);
          PathFinder pathFinder;
          auto path = pathFinder.find(*fromPoint, *toPoint);
          if (path.empty()) {
            SLANG_THROW(std::runtime_error(fmt::format(
                "no path between {} and {}", *fromPointName, *toPointName)));
          }
          for (auto const *n : path) {
            scope.insert(n);
          }
        }
        NetlistDot::render(graph, buffer, scope);
      } else {
        NetlistDot::render(graph, buffer);
      }
      OS::writeFile(*netlistDotFile, buffer.str());
      printStats();
      return 0;
    }

    // Find named nodes by wildcard or regex pattern.
    if (findPattern.has_value() || findRegexPattern.has_value()) {
      auto nodes = findPattern.has_value()
                       ? graph.findNodes(*findPattern)
                       : graph.findNodesRegex(*findRegexPattern);
      if (outputFormat == Format::Json) {
        std::erase_if(nodes, [&](auto *node) {
          return !passesFilters(node->getHierarchicalPath().value_or(""));
        });
        return emitNodeGraph(
            findPattern ? "find" : "find-regex",
            {{"pattern", findPattern ? *findPattern : *findRegexPattern}},
            nodes);
      }
      auto header = Utilities::Row{"ID", "Kind", "Name", "Bounds", "Location"};
      auto table = Utilities::Table{};
      for (auto const *node : nodes) {
        auto path = node->getHierarchicalPath();
        if (!passesFilters(path.value_or(""))) {
          continue;
        }
        auto loc = node->getLocation();
        table.push_back(Utilities::Row{
            std::to_string(node->ID), std::string(toString(node->kind)),
            std::string(path.value_or("(unnamed)")),
            node->getBounds() ? toString(*node->getBounds()) : std::string(),
            loc ? loc->toString(graph.fileTable) : std::string()});
      }
      emitTable(findPattern ? "find" : "find-regex", header, table);
      printStats();
      return outputFormat == Format::Json ? machineExitCode : 0;
    }

    // Report combinational fan-out from a named node.
    if (fanOutName.has_value()) {
      auto [path, range] = parseNameAndRange(*fanOutName);
      if (!graph.hasSignal(path)) {
        SLANG_THROW(
            std::runtime_error(fmt::format("could not find signal: {}", path)));
      }
      auto queryRange = range.value_or(
          DriverBitRange{0, std::numeric_limits<int32_t>::max()});
      auto fanOut =
          graph.getSignalCombFanOut(path, queryRange, maxDepth.value_or(64));
      if (outputFormat == Format::Json && maxDepth.value_or(64) != 0) {
        queryTotal = graph.getSignalCombFanOut(path, queryRange).size();
      }
      if (outputFormat == Format::Json) {
        if (!scopeFilters.empty() || !nameFilters.empty()) {
          std::erase_if(fanOut, [&](auto *node) {
            auto nodePath = node->getHierarchicalPath();
            return nodePath && !passesFilters(*nodePath);
          });
        }
        auto total = queryTotal;
        queryTotal.reset();
        return emitNodeGraph(
            "fan-out",
            {{"signal", *fanOutName}, {"max_depth", maxDepth.value_or(64)}},
            fanOut, total);
      }
      auto header = Utilities::Row{"ID", "Kind", "Name", "Bounds", "Location"};
      auto table = Utilities::Table{};
      for (auto const *n : fanOut) {
        auto path = n->getHierarchicalPath();
        if (path.has_value() && passesFilters(*path)) {
          auto loc = n->getLocation();
          table.push_back(Utilities::Row{
              std::to_string(n->ID), std::string(toString(n->kind)),
              std::string(*path),
              n->getBounds() ? toString(*n->getBounds()) : std::string(),
              loc ? loc->toString(graph.fileTable) : std::string()});
        }
      }
      emitTable("fan-out", header, table);
      printStats();
      return outputFormat == Format::Json ? machineExitCode : 0;
    }

    // Report combinational fan-in to a named node.
    if (fanInName.has_value()) {
      auto [path, range] = parseNameAndRange(*fanInName);
      if (!graph.hasSignal(path)) {
        SLANG_THROW(
            std::runtime_error(fmt::format("could not find signal: {}", path)));
      }
      auto queryRange = range.value_or(
          DriverBitRange{0, std::numeric_limits<int32_t>::max()});
      auto fanIn =
          graph.getSignalCombFanIn(path, queryRange, maxDepth.value_or(64));
      if (outputFormat == Format::Json && maxDepth.value_or(64) != 0) {
        queryTotal = graph.getSignalCombFanIn(path, queryRange).size();
      }
      if (outputFormat == Format::Json) {
        if (!scopeFilters.empty() || !nameFilters.empty()) {
          std::erase_if(fanIn, [&](auto *node) {
            auto nodePath = node->getHierarchicalPath();
            return nodePath && !passesFilters(*nodePath);
          });
        }
        auto total = queryTotal;
        queryTotal.reset();
        return emitNodeGraph(
            "fan-in",
            {{"signal", *fanInName}, {"max_depth", maxDepth.value_or(64)}},
            fanIn, total);
      }
      auto header = Utilities::Row{"ID", "Kind", "Name", "Bounds", "Location"};
      auto table = Utilities::Table{};
      for (auto const *n : fanIn) {
        auto path = n->getHierarchicalPath();
        if (path.has_value() && passesFilters(*path)) {
          auto loc = n->getLocation();
          table.push_back(Utilities::Row{
              std::to_string(n->ID), std::string(toString(n->kind)),
              std::string(*path),
              n->getBounds() ? toString(*n->getBounds()) : std::string(),
              loc ? loc->toString(graph.fileTable) : std::string()});
        }
      }
      emitTable("fan-in", header, table);
      printStats();
      return outputFormat == Format::Json ? machineExitCode : 0;
    }

    // Report the clocks/resets gating a named node. A single hierarchical
    // name can resolve to several nodes (e.g. a register's State node and
    // its same-named output Port), so aggregate sensitivity across all of
    // them and deduplicate.
    if (sensitivityName.has_value()) {
      auto nodes = graph.findNodes(*sensitivityName);
      if (nodes.empty()) {
        SLANG_THROW(std::runtime_error(
            fmt::format("could not find node: {}", *sensitivityName)));
      }
      std::vector<NetlistGraph::SensitivitySource> sensitivity;
      for (auto *node : nodes) {
        for (auto const &src : graph.getSensitivity(*node)) {
          if (std::find(sensitivity.begin(), sensitivity.end(), src) ==
              sensitivity.end()) {
            sensitivity.push_back(src);
          }
        }
      }
      if (outputFormat == Format::Json) {
        Json items = Json::array();
        auto limit = maxResults.value_or(200);
        auto count = limit == 0 ? sensitivity.size()
                                : std::min(sensitivity.size(), size_t(limit));
        for (size_t i = 0; i < count; ++i) {
          items.push_back(
              {{"node", nodeJson(*sensitivity[i].source)},
               {"name",
                sensitivity[i].source->getHierarchicalPath()
                    ? Json(*sensitivity[i].source->getHierarchicalPath())
                    : Json(nullptr)},
               {"edge", ast::toString(sensitivity[i].edgeKind)},
               {"role", "event"}});
        }
        setEnvelope("sensitivity", {{"signal", *sensitivityName}},
                    {{"items", std::move(items)}}, count, sensitivity.size(),
                    count == sensitivity.size());
        printStats();
        return count == sensitivity.size() ? (count == 0 ? 3 : 0) : 4;
      }
      auto header = Utilities::Row{"Name", "Edge", "Location"};
      auto table = Utilities::Table{};
      for (auto const &src : sensitivity) {
        auto path = src.source->getHierarchicalPath();
        if (!passesFilters(path.value_or(""))) {
          continue;
        }
        auto loc = src.source->getLocation();
        table.push_back(Utilities::Row{std::string(path.value_or("(unnamed)")),
                                       std::string(ast::toString(src.edgeKind)),
                                       loc ? loc->toString(graph.fileTable)
                                           : std::string()});
      }
      emitTable("sensitivity", header, table);
      printStats();
      return outputFormat == Format::Json ? machineExitCode : 0;
    }

    // Report the constant values driving a named node.
    if (constantDriversName.has_value()) {
      auto *node = graph.lookup(*constantDriversName);
      if (node == nullptr) {
        SLANG_THROW(std::runtime_error(
            fmt::format("could not find node: {}", *constantDriversName)));
      }
      auto constants = graph.getConstantDrivers(*node);
      if (outputFormat == Format::Json) {
        return emitNodeGraph("constant-drivers",
                             {{"signal", *constantDriversName}}, constants);
      }
      auto header = Utilities::Row{"Value", "Location"};
      auto table = Utilities::Table{};
      for (auto const *n : constants) {
        auto const &constant = n->as<Constant>();
        auto loc = constant.getLocation();
        table.push_back(Utilities::Row{constant.value.toString(),
                                       loc ? loc->toString(graph.fileTable)
                                           : std::string()});
      }
      emitTable("constant-drivers", header, table);
      printStats();
      return outputFormat == Format::Json ? machineExitCode : 0;
    }

    // Report, per bit, the nodes driving a named signal.
    if (driversName.has_value()) {
      auto [path, range] = parseNameAndRange(*driversName);
      if (!graph.hasSignal(path)) {
        SLANG_THROW(
            std::runtime_error(fmt::format("could not find signal: {}", path)));
      }
      // Without an explicit bit range, report the whole signal.
      auto drivers =
          range ? graph.getBitDrivers(path, *range) : graph.getBitDrivers(path);
      if (outputFormat == Format::Json) {
        auto limit = maxResults.value_or(200);
        auto count = limit == 0 ? drivers.size()
                                : std::min(drivers.size(), size_t(limit));
        Json items = Json::array();
        for (size_t i = 0; i < count; ++i) {
          auto const &driver = drivers[i];
          items.push_back({{"bits", toString(driver.bounds)},
                           {"bounds", Json::array({driver.bounds.lower(),
                                                   driver.bounds.upper()})},
                           {"driver", nodeJson(*driver.driver)}});
        }
        setEnvelope("drivers", {{"signal", path}},
                    {{"items", std::move(items)}}, count, drivers.size(),
                    count == drivers.size());
        printStats();
        return count == drivers.size() ? (count == 0 ? 3 : 0) : 4;
      }
      auto header = Utilities::Row{"Bits", "Driver ID", "Driver Kind", "Driver",
                                   "Location"};
      auto table = Utilities::Table{};
      for (auto const &bd : drivers) {
        auto loc = bd.driver->getLocation();
        table.push_back(Utilities::Row{
            toString(bd.bounds), std::to_string(bd.driver->ID),
            std::string(toString(bd.driver->kind)), describeDriver(*bd.driver),
            loc ? loc->toString(graph.fileTable) : std::string()});
      }
      emitTable("drivers", header, table);
      printStats();
      return outputFormat == Format::Json ? machineExitCode : 0;
    }

    // Find a point-to-point path in the netlist.
    if (fromPointName.has_value() && toPointName.has_value()) {
      std::vector<NetlistNode *> fromPoints;
      std::vector<NetlistNode *> toPoints;
      auto signalEndpoints = outputFormat == Format::Json && !fromNodeId &&
                             !toNodeId && !fromKind && !toKind;
      if (signalEndpoints) {
        auto [fromSignal, fromRange] = parseNameAndRange(*fromPointName);
        auto [toSignal, toRange] = parseNameAndRange(*toPointName);
        auto sourceRange = fromRange.value_or(
            DriverBitRange{0, std::numeric_limits<int32_t>::max()});
        auto targetRange = toRange.value_or(
            DriverBitRange{0, std::numeric_limits<int32_t>::max()});
        std::unordered_set<NetlistNode *> seen;
        for (auto const &node : graph) {
          for (auto const &edge : node->getOutEdges()) {
            if (!edge->disabled && edge->symbol != nullptr &&
                edge->symbol->hierarchicalPath == fromSignal &&
                edge->bounds.overlaps(sourceRange) &&
                seen.insert(&edge->getTargetNode()).second) {
              fromPoints.push_back(&edge->getTargetNode());
            }
          }
        }
        seen.clear();
        for (auto const &driver : graph.getBitDrivers(toSignal, targetRange)) {
          if (seen.insert(driver.driver).second)
            toPoints.push_back(driver.driver);
        }
        if (fromPoints.empty() || toPoints.empty()) {
          SLANG_THROW(std::runtime_error("could not resolve signal endpoint"));
        }
      } else {
        fromPoints.push_back(
            resolvePathNode(*fromPointName, fromNodeId, fromKind, "from"));
        toPoints.push_back(
            resolvePathNode(*toPointName, toNodeId, toKind, "to"));
      }

      DEBUG_PRINT("Searching for path between: {} and {}\n", *fromPointName,
                  *toPointName);

      // Search for the path.
      PathFinder pathFinder;
      NetlistPath path;
      for (auto *fromPoint : fromPoints) {
        for (auto *toPoint : toPoints) {
          path = crossStatePolicy == "never"
                     ? pathFinder.findComb(*fromPoint, *toPoint)
                     : pathFinder.find(*fromPoint, *toPoint);
          if (!path.empty())
            break;
        }
        if (!path.empty())
          break;
      }

      if (crossStatePolicy == "once" &&
          std::ranges::count_if(path, [](auto const *node) {
            return node->kind == NodeKind::State;
          }) > 1) {
        path = {};
      }

      if (!path.empty()) {
        if (outputFormat == Format::Json) {
          Json nodes = Json::array();
          Json edges = Json::array();
          for (size_t i = 0; i < path.size(); ++i) {
            nodes.push_back(nodeJson(*path[i]));
            if (i + 1 < path.size()) {
              auto edge = path[i]->findEdgeTo(*path[i + 1]);
              if (edge != path[i]->end())
                edges.push_back(edgeJson(**edge));
            }
          }
          setEnvelope("path",
                      {{"from", *fromPointName},
                       {"to", *toPointName},
                       {"cross_state", crossStatePolicy}},
                      {{"nodes", std::move(nodes)},
                       {"edges", std::move(edges)},
                       {"boundaries", Json::array()}},
                      path.size(), path.size());
          printStats();
          return 0;
        }
        auto result = reportPath(graph.fileTable, diagnostics.get(), path);
        OS::print(fmt::format("{}\n", result));
        printStats();
        return 0;
      }

      // No path found.
      if (outputFormat == Format::Json) {
        Json boundaries = Json::array();
        if (crossStatePolicy == "never") {
          for (auto *fromPoint : fromPoints) {
            for (auto *toPoint : toPoints) {
              auto structural = pathFinder.find(*fromPoint, *toPoint);
              for (auto const *node : structural) {
                if (node->kind == NodeKind::State) {
                  boundaries.push_back(nodeJson(*node));
                  break;
                }
              }
              if (!boundaries.empty())
                break;
            }
            if (!boundaries.empty())
              break;
          }
        }
        setEnvelope("path",
                    {{"from", *fromPointName},
                     {"to", *toPointName},
                     {"cross_state", crossStatePolicy}},
                    {{"nodes", Json::array()},
                     {"edges", Json::array()},
                     {"boundaries", std::move(boundaries)}},
                    0, 0);
        printStats();
        return 3;
      }
      SLANG_THROW(std::runtime_error(fmt::format(
          "no path between {} and {}", *fromPointName, *toPointName)));
    }

    // If we reach here, no action was specified.
    SLANG_THROW(std::runtime_error("no action specified"));
  }
  SLANG_CATCH(const std::exception &e) {
    if (outputFormat == Format::Json) {
      auto message = std::string(e.what());
      auto ambiguous = message.starts_with("ambiguous ");
      Json envelope = {
          {"schema_version", 1},
          {"tool",
           {{"name", "slang-netlist"},
            {"version", SLANG_NETLIST_VERSION},
            {"graph_schema_version", NetlistSerializer::formatVersion}}},
          {"command", "error"},
          {"query", Json::object()},
          {"data", Json::object()},
          {"diagnostics",
           Json::array(
               {{{"severity", "error"},
                 {"code", ambiguous ? "ambiguous_endpoint" : "invalid_query"},
                 {"message", message}}})},
          {"summary",
           {{"status", "error"},
            {"complete", true},
            {"returned", 0},
            {"total", 0}}}};
      writeOutput(envelope.dump(2) + "\n");
      return ambiguous ? 7 : 6;
    }
    SLANG_REPORT_EXCEPTION(e, "{}\n");
    return 1;
  }

  return 0;
}
