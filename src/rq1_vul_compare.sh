#!/bin/bash

for sweep in 0 1 2 3 4 5 6 7 8 9 10 11 12 13 14 15 16 17 18 19;do
  echo parameter sweep $sweep
  echo "Source domain training"
  lr=$(grep 'learning_rate:' /cephyr/NOBACKUP/groups/snic2021-23-24/vulnerability_repair/major_revision_2021_09/only_first_line_big_vul_random_fine_tune_context3_models/${sweep}_parameter_sweep/train_config.yml)
  rs=$(grep 'rnn_size:' /cephyr/NOBACKUP/groups/snic2021-23-24/vulnerability_repair/major_revision_2021_09/only_first_line_big_vul_random_fine_tune_context3_models/${sweep}_parameter_sweep/train_config.yml)
  echo "${lr}, ${rs}"
  python3 compare.py --tgt /cephyr/NOBACKUP/groups/snic2021-23-24/vulnerability_repair/major_revision_2021_09/only_first_line_context3_models/big_vul_random_test_data/big_vul_random_fine_tune_test.tgt.txt --src /cephyr/NOBACKUP/groups/snic2021-23-24/vulnerability_repair/major_revision_2021_09/only_first_line_context3_models/${sweep}_parameter_sweep/big_vul_random_fine_tune_test.src.txt.predictions
  echo "Target domain training"
  lr=$(grep 'learning_rate:' /cephyr/NOBACKUP/groups/snic2021-23-24/vulnerability_repair/major_revision_2021_09/only_first_line_big_vul_random_fine_tune_context3_models/${sweep}_parameter_sweep/train_config.yml)
  rs=$(grep 'rnn_size:' /cephyr/NOBACKUP/groups/snic2021-23-24/vulnerability_repair/major_revision_2021_09/only_first_line_big_vul_random_fine_tune_context3_models/${sweep}_parameter_sweep/train_config.yml)
  echo "${lr}, ${rs}"
  python3 compare.py --tgt /cephyr/NOBACKUP/groups/snic2021-23-24/vulnerability_repair/major_revision_2021_09/only_first_line_big_vul_random_fine_tune_context3_models/big_vul_random_test_data/big_vul_random_fine_tune_test.tgt.txt --src /cephyr/NOBACKUP/groups/snic2021-23-24/vulnerability_repair/major_revision_2021_09/only_first_line_big_vul_random_fine_tune_context3_models/${sweep}_parameter_sweep/big_vul_random_fine_tune_test.src.txt.predictions
  echo
done
