#!/bin/bash

echo "Time split"
for sweep in 0 1 2 3 4 5 6 7 8 9 10 11 12 13 14 15 16 17 18 19;do
  echo parameter sweep $sweep
  echo "Target domain training"
  lr=$(grep 'learning_rate:' /cephyr/NOBACKUP/groups/snic2021-23-24/vulnerability_repair/only_first_line_year_fine_tune_context3_models/${sweep}_parameter_sweep/train_config.yml)
  rs=$(grep 'rnn_size:' /cephyr/NOBACKUP/groups/snic2021-23-24/vulnerability_repair/only_first_line_year_fine_tune_context3_models/${sweep}_parameter_sweep/train_config.yml)
  echo "${lr}, ${rs}"
  python3 compare.py --tgt /cephyr/NOBACKUP/groups/snic2021-23-24/vulnerability_repair/only_first_line_context3_models/year_test_data/YearBugFix_test_tgt.txt --src /cephyr/NOBACKUP/groups/snic2021-23-24/vulnerability_repair/only_first_line_year_fine_tune_context3_models/${sweep}_parameter_sweep/YearBugFix_test_src.txt.predictions
  echo "Transfer learning"
  lr=$(grep 'learning_rate:' /cephyr/NOBACKUP/groups/snic2021-23-24/vulnerability_repair/only_first_line_context3_models/${sweep}_parameter_sweep/train_config.yml)
  rs=$(grep 'rnn_size:' /cephyr/NOBACKUP/groups/snic2021-23-24/vulnerability_repair/only_first_line_context3_models/${sweep}_parameter_sweep/train_config.yml)	
  echo "${lr}, ${rs}"
  python3 compare.py --tgt /cephyr/NOBACKUP/groups/snic2021-23-24/vulnerability_repair/only_first_line_context3_models/year_test_data/YearBugFix_test_tgt.txt --src /cephyr/NOBACKUP/groups/snic2021-23-24/vulnerability_repair/only_first_line_context3_models/${sweep}_parameter_sweep/year_fine_tune/YearBugFix_test_src.txt.predictions
  echo
done
echo
echo "CWE split"
for sweep in 0 1 2 3 4 5 6 7 8 9 10 11 12 13 14 15 16 17 18 19;do
  echo parameter sweep $sweep
  echo "Target domain training"
  lr=$(grep 'learning_rate:' /cephyr/NOBACKUP/groups/snic2021-23-24/vulnerability_repair/only_first_line_cwe_fine_tune_context3_models/${sweep}_parameter_sweep/train_config.yml)
  rs=$(grep 'rnn_size:' /cephyr/NOBACKUP/groups/snic2021-23-24/vulnerability_repair/only_first_line_cwe_fine_tune_context3_models/${sweep}_parameter_sweep/train_config.yml)
  echo "${lr}, ${rs}"
  python3 compare.py --tgt /cephyr/NOBACKUP/groups/snic2021-23-24/vulnerability_repair/only_first_line_context3_models/cwe_test_data/CWEBugFix_test_tgt.txt --src /cephyr/NOBACKUP/groups/snic2021-23-24/vulnerability_repair/only_first_line_cwe_fine_tune_context3_models/${sweep}_parameter_sweep/CWEBugFix_test_src.txt.predictions
  echo "Transfer learning"
  lr=$(grep 'learning_rate:' /cephyr/NOBACKUP/groups/snic2021-23-24/vulnerability_repair/only_first_line_context3_models/${sweep}_parameter_sweep/train_config.yml)
  rs=$(grep 'rnn_size:' /cephyr/NOBACKUP/groups/snic2021-23-24/vulnerability_repair/only_first_line_context3_models/${sweep}_parameter_sweep/train_config.yml)  
  echo "${lr}, ${rs}"
  python3 compare.py --tgt /cephyr/NOBACKUP/groups/snic2021-23-24/vulnerability_repair/only_first_line_context3_models/cwe_test_data/CWEBugFix_test_tgt.txt --src /cephyr/NOBACKUP/groups/snic2021-23-24/vulnerability_repair/only_first_line_context3_models/${sweep}_parameter_sweep/cwe_fine_tune/CWEBugFix_test_src.txt.predictions
  echo
done
