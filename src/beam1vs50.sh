#!/bin/bash

for sweep in 0 1 2 3 4 5 6 7 8 9 10 11 12 13 14 15 16 17 18 19;do
  echo parameter sweep $sweep
  echo "Source domain training"
  lr=$(grep 'learning_rate:' /cephyr/NOBACKUP/groups/snic2021-23-24/vulnerability_repair/only_first_line_context3_models/${sweep}_parameter_sweep/train_config.yml)
  rs=$(grep 'rnn_size:' /cephyr/NOBACKUP/groups/snic2021-23-24/vulnerability_repair/only_first_line_context3_models/${sweep}_parameter_sweep/train_config.yml)
  echo "${lr}, ${rs}"
  python3 compare.py --tgt /cephyr/NOBACKUP/groups/snic2021-23-24/vulnerability_repair/only_first_line_context3_models/random_test_data/RandomBugFix_test_tgt.txt --src /cephyr/NOBACKUP/groups/snic2021-23-24/vulnerability_repair/only_first_line_context3_models/${sweep}_parameter_sweep/RandomBugFix_test_src.txt.predictions
  echo "Transfer learning"
  perl -ne '((($. -1) % 50) < 1) && print' /cephyr/NOBACKUP/groups/snic2021-23-24/vulnerability_repair/only_first_line_context3_models/${sweep}_parameter_sweep/random_fine_tune/RandomBugFix_test_src.txt.predictions > /cephyr/NOBACKUP/groups/snic2021-23-24/vulnerability_repair/only_first_line_context3_models/${sweep}_parameter_sweep/random_fine_tune/RandomBugFix_test_src.txt.beam1.predictions
  python3 compare.py --tgt /cephyr/NOBACKUP/groups/snic2021-23-24/vulnerability_repair/only_first_line_context3_models/random_test_data/RandomBugFix_test_tgt.txt --src /cephyr/NOBACKUP/groups/snic2021-23-24/vulnerability_repair/only_first_line_context3_models/${sweep}_parameter_sweep/random_fine_tune/RandomBugFix_test_src.txt.beam1.predictions
  echo
done
