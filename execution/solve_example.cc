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
#include "absl/strings/str_format.h"
#include "absl/types/span.h"
#include "contest_problem.pb.h"
#include "execution/py_locations.h"
#include "execution/py_tester_sandboxer.h"
#include "execution/status_macros.h"
#include "execution/tester_sandboxer.h"
#include "riegeli/bytes/fd_reader.h"
#include "riegeli/records/record_reader.h"
#include "execution/json.hpp"

ABSL_FLAG(std::string, data_path, "", "Path to folder with dataset.");
ABSL_FLAG(std::string, valid_path, "", "Path to validation dataset.");
ABSL_FLAG(std::string, input_path, "", "Path to input dataset.");
ABSL_FLAG(std::string, output_path, "", "Path to output file.");

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

    std::vector<absl::string_view> GetLangSolutions(const ContestProblem &problem,
                                                    int max_size, deepmind::code_contests::ContestProblem_Solution::Language lang)
    {
      std::vector<absl::string_view> solutions;
      // solutions.push_back(kGoodSolution);
      for (const auto &test : problem.solutions())
      {
        if (!test.has_solution())
        {
          continue;
        }
        if (test.language() == lang)
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
        cout<<"compilation error"<<endl;
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
      string id;
      string generated;
      string path;
      bool evaluated;
      bool passed;

      CandidateSolution(string id, string generated, string path, bool evaluated, bool passed) : id(id), generated(generated), path(path), evaluated(evaluated), passed(passed) {}
    };

    absl::Status SolveAll(vector<string> filenames,
                          const std::string input_path, const std::string output_path)
    {
      // set up evaluation environment
      Py3TesterSandboxer tester3(Py3InterpreterPath(), Py3LibraryPaths());
      Py2TesterSandboxer tester2(Py2InterpreterPath(), Py2LibraryPaths());
      TestOptions options;
      options.max_execution_duration = absl::Seconds(5);
      options.num_threads = 1;
      options.stop_on_first_failure = true;

      // parse JSON inputs
      std::ifstream input_file(input_path);
      json data = json::parse(input_file);
      input_file.close();
      vector<CandidateSolution> generated_solutions;

      for (auto entry : data.items())
      {
        string path = entry.key();
        cout << path << endl;
        vector<json> generations = entry.value();
        for (const auto &g : generations)
        {
          string id = g["id"];
          for (const auto &solution : g["model_completions"])
          {
            // string solution = g["generated"];
            CandidateSolution cs(id, solution, path, false, false);
            generated_solutions.push_back(cs);
          }
        }
      }
      cout << "parsed the input json" << endl;

      // we will write the output json to this
      vector<json> test_results;

      // go through all the riegeli files in this dataset
      for (const auto &filename : filenames)
      {
        // iterate through problems
        riegeli::RecordReader<riegeli::FdReader<>> reader(
            std::forward_as_tuple(filename));
        ContestProblem problem;
        vector<tuple<int, int>> passes_and_fails;
        while (reader.ReadRecord(problem))
        {
          vector<CandidateSolution> generated_for_this_problem;
          bool found = false;
          for (const auto &s : generated_solutions)
          {
            if (s.id == problem.name())
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

            ASSIGN_OR_RETURN(MultiTestResult result3,
                             tester3.Test(solution, inputs, options, outputs));
            // ReportResults(result);
            bool passed3 = DidItPass(result3);
            bool passed2 = false;
            if (!passed3)
            {
              ASSIGN_OR_RETURN(MultiTestResult result2,
                               tester2.Test(solution, inputs, options, outputs));
              // ReportResults(result);
              passed2 = DidItPass(result2);
            }

            bool passed = passed3 || passed2;

            json res;
            res["id"] = g.id;
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
      }

      json final_output;
      final_output["results"] = test_results;

      cout << "writing output to: " << output_path << endl;
      std::ofstream output_file(output_path);
      output_file << final_output.dump();
      output_file.flush();
      output_file.close();

      return absl::OkStatus();
    }

    // this method iterates through a dataset and evaluates the reference solutions against the given tests
    absl::Status SolveReferenceSolution(const vector<string> filenames)
    {

      // set up evaluation environment
      Py3TesterSandboxer tester3(Py3InterpreterPath(), Py3LibraryPaths());
      Py2TesterSandboxer tester2(Py2InterpreterPath(), Py2LibraryPaths());
      TestOptions options;
      options.max_execution_duration = absl::Seconds(5);
      options.num_threads = 2;
      options.stop_on_first_failure = true;

      // the problem descriptions are split over multiple riegeli files
      for (const auto &filename : filenames)
      {

        // iterate through problems
        riegeli::RecordReader<riegeli::FdReader<>> reader(
            std::forward_as_tuple(filename));
        ContestProblem problem;
        vector<tuple<int, int>> passes_and_fails;
        while (reader.ReadRecord(problem))
        {
          // const auto name = "1091_G. New Year and the Factorisation Collaboration";
          // const auto name = "1575_A. Another Sorting Problem";
          const auto name = problem.name();
          // cout<<name<<endl;
          if (
              !((name == "1569_A. Balanced Substring") ||
                (name == "1551_D2. Domino (hard version)") ||
                (name == "1552_E. Colors and Intervals") ||
                (name == "1557_E. Assiut Chess")))
          {
            continue;
          }
          std::cout << "found the problem" << std::endl;
          cout << name << endl;
          cout << "-----------------" << endl;
          cout << problem.name() << endl;
          const auto start = absl::Now();
          const std::vector<absl::string_view> inputs =
              GetInputs(problem,
                        /*max_size=*/-1); // -1 for no resizing
          const std::vector<absl::string_view> outputs =
              GetOutputs(problem,
                         /*max_size=*/-1);

          // get solutions for python2 and 3 and concatenate them into one vector
          const std::vector<absl::string_view> py2_solutions =
              GetLangSolutions(problem,
                               /*max_size=*/-1, deepmind::code_contests::ContestProblem_Solution::Language::ContestProblem_Solution_Language_PYTHON);
          std::vector<absl::string_view> solutions = GetLangSolutions(problem,
                                                                      /*max_size=*/-1, deepmind::code_contests::ContestProblem_Solution::Language::ContestProblem_Solution_Language_PYTHON3);
          copy(begin(py2_solutions), end(py2_solutions), back_inserter(solutions));

          int num_passed = 0;
          int num_failed = 0;

          // how many solutions do we want to evaluate at most:
          int max_per_problem = 50;
          std::vector<bool> passorfail;
          for (const auto &solution : solutions)
          {
            ASSIGN_OR_RETURN(MultiTestResult result3,
                             tester3.Test(solution, inputs, options, outputs));
            // ReportResults(result);
            bool passed3 = DidItPass(result3);
            bool passed2 = false;
            if (!passed3)
            {
              ASSIGN_OR_RETURN(MultiTestResult result2,
                               tester2.Test(solution, inputs, options, outputs));
              // ReportResults(result);
              passed2 = DidItPass(result2);
            }

            bool passed = passed3 || passed2;
            passorfail.push_back(passed);
            if (passed)
            {
              num_passed++;
            }
            else
            {
              num_failed++;
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

          // for (const auto &t : passorfail)
          //{
          //   std::cout << t << std::endl;
          // }
          passes_and_fails.push_back(std::tuple<int, int>{num_passed, num_failed});

          // std::cout << "num passed: " << num_passed << ", num failed: " << num_failed << std::endl;
        }

        for (const auto &p_and_f : passes_and_fails)
        {

          std::cout << std::get<0>(p_and_f) << "," << std::get<1>(p_and_f) << std::endl;
        }
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
  const auto data_path = absl::GetFlag(FLAGS_data_path);

  vector<string> problem_filenames;
  problem_filenames.push_back(data_path + "dm-code_contests/code_contests_test.riegeli");
  problem_filenames.push_back(data_path + "dm-code_contests/code_contests_valid.riegeli");
  for (int i = 0; i <= 128; i++)
  {
    const auto str = absl::StrFormat("%0*d", 5, i);
    const auto complete_path = data_path + "dm-code_contests/code_contests_train.riegeli-" + str + "-of-00128";
    problem_filenames.push_back(complete_path);
  }

  if (absl::Status status = deepmind::code_contests::SolveAll(
          problem_filenames,
          absl::GetFlag(FLAGS_input_path),
          absl::GetFlag(FLAGS_output_path));
      !status.ok())
  {
    std::cerr << "Failed: " << status.message() << std::endl;
  }
}
