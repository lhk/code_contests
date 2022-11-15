// Copyright 2022 DeepMind Technologies Limited
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//      http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

// A simple utility that prints the names of the problems in a dataset. If
// provided multiple filenames as arguments, these are read sequentially.

#include <fcntl.h>

#include <functional>
#include <iostream>
#include <fstream>
#include <string>
#include <tuple>
#include <vector>

#include "absl/flags/parse.h"
#include "absl/flags/flag.h"
#include "absl/status/status.h"
#include "absl/status/statusor.h"
#include "absl/strings/string_view.h"
#include "absl/types/span.h"
#include "contest_problem.pb.h"
#include "execution/py_locations.h"
#include "execution/py_tester_sandboxer.h"
#include "execution/status_macros.h"
#include "execution/tester_sandboxer.h"
#include "riegeli/bytes/fd_reader.h"
#include "riegeli/records/record_reader.h"
#include "execution/json.hpp"

ABSL_FLAG(std::string, valid_path, "", "Path to validation dataset.");
ABSL_FLAG(std::string, input_path, "", "Path to input dataset.");

using json = nlohmann::json;
using namespace std;
namespace deepmind::code_contests
{
  namespace
  {

    constexpr absl::string_view kGoodSolution = R"py(
# good solution
t = int(input())
while t:
  n = int(input())
  print(2, n-1)
  t -= 1
)py";

    constexpr absl::string_view kBadSolution = R"py(
t = int(input())
while t:
  n = int(input())
  if n > 20:
    print(1, 1)
  else:
    print(2, n-1)
  t -= 1
)py";

    constexpr absl::string_view kInvalidSolution = ")";

    absl::StatusOr<ContestProblem> FindGregorAndCryptography(
        const absl::string_view filename)
    {
      riegeli::RecordReader<riegeli::FdReader<>> reader(
          std::forward_as_tuple(filename));
      ContestProblem problem;
      while (reader.ReadRecord(problem))
      {
        if (problem.name() == "1549_A. Gregor and Cryptography")
          return problem;
      }
      return absl::NotFoundError(
          "Gregor and Cryptography problem not found. Did you pass the "
          "validation dataset?");
    }

    std::vector<absl::string_view> GetInputs(const ContestProblem &problem,
                                             int max_size)
    {
      std::vector<absl::string_view> inputs;
      for (const auto &test : problem.public_tests())
      {
        inputs.push_back(test.input());
      }
      for (const auto &test : problem.private_tests())
      {
        inputs.push_back(test.input());
      }
      for (const auto &test : problem.generated_tests())
      {
        inputs.push_back(test.input());
      }

      if (max_size > -1)
      {
        inputs.resize(max_size);
      }
      return inputs;
    }

    std::vector<absl::string_view> GetPython3Solutions(const ContestProblem &problem,
                                                       int max_size)
    {
      std::vector<absl::string_view> solutions;
      // solutions.push_back(kGoodSolution);
      for (const auto &test : problem.solutions())
      {
        if (!test.has_solution())
        {
          continue;
        }
        if (test.language() == test.PYTHON3)
        {
          solutions.push_back(test.solution());
        }
      }
      if (max_size > -1)
      {
        solutions.resize(max_size);
      }
      return solutions;
    }

    std::vector<absl::string_view> GetOutputs(const ContestProblem &problem,
                                              int max_size)
    {
      std::vector<absl::string_view> outputs;
      for (const auto &test : problem.public_tests())
      {
        outputs.push_back(test.output());
      }
      for (const auto &test : problem.private_tests())
      {
        outputs.push_back(test.output());
      }
      for (const auto &test : problem.generated_tests())
      {
        outputs.push_back(test.output());
      }
      if (max_size > -1)
      {
        outputs.resize(max_size);
      }
      return outputs;
    }

    void ReportResults(const MultiTestResult &multi_result)
    {
      std::cout << "Compilation "
                << (multi_result.compilation_result.program_status ==
                            ProgramStatus::kSuccess
                        ? "succeeded"
                        : "failed")
                << "\n";
      int i = 0;
      for (const auto &test_result : multi_result.test_results)
      {
        if (!test_result.passed.has_value())
        {
          std::cout << "Test " << i << " did not run.\n";
        }
        else if (*test_result.passed)
        {
          std::cout << "Test " << i << " passed.\n";
        }
        else
        {
          std::cout << "Test " << i << " failed.\n";
        }
        ++i;
      }
    }

    bool DidItPass(const MultiTestResult &multi_result)
    {
      if (!(multi_result.compilation_result.program_status ==
            ProgramStatus::kSuccess))
      {
        return false;
      }
      for (const auto &test_result : multi_result.test_results)
      {
        if (!test_result.passed.has_value())
        {
          return false;
        }
        else if (*test_result.passed)
        {
          // nothing to do here
        }
        else
        {
          return false;
        }
      }
      return true;
    }

    struct CandidateSolution
    {
      int idx;
      string name;
      string generated;
      bool evaluated;
      bool passed;

      CandidateSolution(int idx, string name, string generated, bool evaluated, bool passed) : idx(idx), name(name), generated(generated), evaluated(evaluated), passed(passed) {}
    };

    absl::Status SolveAll(const absl::string_view valid_path, const std::string input_path)
    {
      // parse JSON inputs
      std::ifstream input_file(input_path);
      json data = json::parse(input_file);
      input_file.close();
      vector<CandidateSolution> generated_solutions;

      vector<json> generations = data["generations"];
      for (const auto &g : generations)
      {
        int idx = g["idx"];
        string name = g["name"];
        string solution = g["generated"];
        CandidateSolution cs(idx, name, solution, false, false);
        generated_solutions.push_back(cs);
      }
      cout << "parsed the input json" << endl;

      // set up evaluation environment
      Py3TesterSandboxer tester(Py3InterpreterPath(), Py3LibraryPaths());
      TestOptions options;
      options.max_execution_duration = absl::Seconds(5);
      options.num_threads = 12;
      options.stop_on_first_failure = true;

      // iterate through problems
      riegeli::RecordReader<riegeli::FdReader<>> reader(
          std::forward_as_tuple(valid_path));
      ContestProblem problem;

      vector<json> test_results;
      while (reader.ReadRecord(problem))
      {
        cout << problem.name() << endl;
        vector<CandidateSolution> generated_for_this_problem;
        bool found = false;
        for (const auto &s : generated_solutions)
        {
          if (s.name == problem.name())
          {
            generated_for_this_problem.push_back(s);
            found = true;
          }
        }
        if (found)
        {
          cout << "found a generation" << endl;
        }
        else
        {
          continue;
        }

        const std::vector<absl::string_view> inputs =
            GetInputs(problem,
                      /*max_size=*/-1); // -1 for no resizing
        const std::vector<absl::string_view> outputs =
            GetOutputs(problem,
                       /*max_size=*/-1);

        std::vector<int> passorfail;
        for (const auto &g : generated_for_this_problem)
        {
          std::string solution = g.generated;
          cout << solution << endl;
          ASSIGN_OR_RETURN(MultiTestResult result,
                           tester.Test(solution, inputs, options, outputs));

          // ReportResults(result);
          bool passed = DidItPass(result);
          passorfail.push_back(passed);

          json res;
          res["name"] = g.name;
          res["idx"] = g.idx;
          res["generated"] = g.generated;
          res["passed"] = passed;
          test_results.push_back(res);

          if (passed)
          {
            std::cout << "passed" << std::endl;
          }
          else
          {
            std::cout << "failed" << std::endl;
          }
        }
      }

      json final_output;
      final_output["results"] = test_results;
      cout << "--------------------------" << endl;
      cout << final_output.dump() << endl;

      return absl::OkStatus();
    }

    absl::Status SolveValid(const absl::string_view valid_path)
    {

      // set up evaluation environment
      Py3TesterSandboxer tester(Py3InterpreterPath(), Py3LibraryPaths());
      TestOptions options;
      options.max_execution_duration = absl::Seconds(5);
      options.num_threads = 12;
      options.stop_on_first_failure = true;

      // iterate through problems
      riegeli::RecordReader<riegeli::FdReader<>> reader(
          std::forward_as_tuple(valid_path));
      ContestProblem problem;

      int num_problems = 0;
      std::vector<std::tuple<int, int>> passes_and_fails;
      while (reader.ReadRecord(problem))
      {
        if (problem.name() != "1549_A. Gregor and Cryptography")
        {
          continue;
        }
        std::cout << "found the problem" << std::endl;

        const auto start = absl::Now();
        const std::vector<absl::string_view> inputs =
            GetInputs(problem,
                      /*max_size=*/-1); // -1 for no resizing
        const std::vector<absl::string_view> outputs =
            GetOutputs(problem,
                       /*max_size=*/-1);

        const std::vector<absl::string_view> solutions =
            GetPython3Solutions(problem,
                                /*max_size=*/-1);

        int num_passed = 0;
        int num_failed = 0;

        // if we want to evaluate only a subset
        int max_per_problem = 50;
        std::vector<int> passorfail;
        for (const auto &solution : solutions)
        {
          ASSIGN_OR_RETURN(MultiTestResult result,
                           tester.Test(solution, inputs, options, outputs));

          // ReportResults(result);
          passorfail.push_back(DidItPass(result));
          if (DidItPass(result))
          {
            num_passed++;
            // std::cout << "passed" << std::endl;
          }
          else
          {
            // ReportResults(result);
            num_failed++;
            // std::cout << "failed" << std::endl;
          }
          if (num_passed + num_failed >= max_per_problem)
          {
            std::cout << solution << std::endl;
            break;
          }
        }
        std::cout << "num passed: " << num_passed << ", num failed: " << num_failed << std::endl;

        const auto stop = absl::Now();
        std::cout << "Total duration: " << stop - start << std::endl;

        for (const auto &t : passorfail)
        {
          std::cout << t << std::endl;
        }
        // passes_and_fails.push_back(std::tuple<int, int>{num_passed, num_failed});

        // std::cout << "num passed: " << num_passed << ", num failed: " << num_failed << std::endl;
      }

      for (const auto &p_and_f : passes_and_fails)
      {

        std::cout << std::get<0>(p_and_f) << "," << std::get<1>(p_and_f) << std::endl;
      }

      return absl::OkStatus();
    }

    absl::Status SolveGregorAndCryptography(
        const absl::string_view valid_filename)
    {
      ASSIGN_OR_RETURN(ContestProblem gregor_and_cryptography,
                       FindGregorAndCryptography(valid_filename));
      const std::vector<absl::string_view> inputs =
          GetInputs(gregor_and_cryptography,
                    /*max_size=*/10);
      const std::vector<absl::string_view> outputs =
          GetOutputs(gregor_and_cryptography,
                     /*max_size=*/10);

      Py3TesterSandboxer tester(Py3InterpreterPath(), Py3LibraryPaths());
      TestOptions options;
      options.num_threads = 4;
      options.stop_on_first_failure = true;

      std::cout << R"(We will try to solve "Gregor and Cryptography":
https://codeforces.com/problemset/problem/1549/A

We will run:
  1. A program that does not compile.
  2. A program that runs successfully, but gives the wrong answer sometimes.
  3. A correct solution.

--------------------------------------------------------------------------------
An invalid program is reported as not compiling:

)";
      ASSIGN_OR_RETURN(MultiTestResult invalid_result,
                       tester.Test(kInvalidSolution, inputs, options, outputs));
      ReportResults(invalid_result);
      std::cout << "sandbox result status" << invalid_result.compilation_result.SandboxResultStatus().ok() << std::endl;

      std::cout << R"(
--------------------------------------------------------------------------------
The bad solution passes a few tests but then fails.
Because we set stop_on_first_failure to True, we stop once we see a failure.
We are running on 4 threads, so it's possible that more than one failure occurs
before all threads stop.

)";
      ASSIGN_OR_RETURN(MultiTestResult bad_result,
                       tester.Test(kBadSolution, inputs, options, outputs));
      ReportResults(bad_result);
      std::cout << "sandbox result status" << bad_result.compilation_result.SandboxResultStatus().ok() << std::endl;
      for (const auto &res : bad_result.test_results)
      {
        std::cout << res.SandboxResultStatus().ok() << std::endl;
        std::cout << res.sandbox_result << std::endl;
      }
      std::cout << R"(
--------------------------------------------------------------------------------
The good solution passes all tests.

)";

      ASSIGN_OR_RETURN(MultiTestResult good_result,
                       tester.Test(kGoodSolution, inputs, options, outputs));
      ReportResults(good_result);
      std::cout << "sandbox result status" << good_result.compilation_result.SandboxResultStatus().ok() << std::endl;

      return absl::OkStatus();
    }

  } // namespace
} // namespace deepmind::code_contests

int main(int argc, char *argv[])
{
  absl::ParseCommandLine(argc, argv);
  std::cout << "starting" << std::endl;

  if (absl::Status status = deepmind::code_contests::SolveAll(
          absl::GetFlag(FLAGS_valid_path), absl::GetFlag(FLAGS_input_path));
      !status.ok())
  {
    std::cerr << "Failed: " << status.message() << std::endl;
  }
}
