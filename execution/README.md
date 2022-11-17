```
gsutil -m cp -r gs://dm-code_contests /home/lhk/data/

bazel run -c opt execution:solve_example -- --data_path=/home/lhk/data/ --input_path=/home/lhk/data/cc_generations/cc_generations.json --output_path=/home/lhk/data/cc_generations/results.json   --python3_path=/usr/bin/python3.10 --python3_library_paths=/usr/lib/python3.10```