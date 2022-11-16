```
gsutil -m cp -r gs://dm-code_contests /tmp

bazel run -c opt execution:solve_example -- --input_path=/home/lhk/programming/code_contests/execution/sample.json  --valid_path=/tmp/dm-code_contests/code_contests_test.riegeli   --python3_path=/usr/bin/python3.10 --python3_library_paths=/usr/lib/python3.10```