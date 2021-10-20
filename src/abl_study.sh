#!/bin/bash

for sweep in 0 1 2 3 4 5 6 7 8 9 10 11 12 13 14 15 16 17 18 19 20 21 22 23 24 25 26 27 28 29 30 31;do
  echo parameter sweep $sweep
  lr=$(grep 'learning_rate:' /cephyr/NOBACKUP/groups/snic2021-23-24/vulnerability_repair/major_revision_2021_09/only_first_line_context3_more_parameters_models/${sweep}_parameter_sweep/train_config.yml)
  rs=$(grep 'rnn_size:' /cephyr/NOBACKUP/groups/snic2021-23-24/vulnerability_repair/major_revision_2021_09/only_first_line_context3_more_parameters_models/${sweep}_parameter_sweep/train_config.yml)
  do=$(grep 'dropout:' /cephyr/NOBACKUP/groups/snic2021-23-24/vulnerability_repair/major_revision_2021_09/only_first_line_context3_more_parameters_models/${sweep}_parameter_sweep/train_config.yml)
  vs=$(grep 'src_vocab_size:' /cephyr/NOBACKUP/groups/snic2021-23-24/vulnerability_repair/major_revision_2021_09/only_first_line_context3_more_parameters_models/${sweep}_parameter_sweep/train_config.yml)
  l=$(grep 'enc_layers:' /cephyr/NOBACKUP/groups/snic2021-23-24/vulnerability_repair/major_revision_2021_09/only_first_line_context3_more_parameters_models/${sweep}_parameter_sweep/train_config.yml)
  echo "${lr}, ${rs}, ${do}, ${vs}, ${l}"
  echo "Source domain training"
  python3 compare.py --tgt /cephyr/NOBACKUP/groups/snic2021-23-24/vulnerability_repair/major_revision_2021_09/only_first_line_context3_more_parameters_models/big_vul_random_test_data/big_vul_random_fine_tune_test.tgt.txt --src /cephyr/NOBACKUP/groups/snic2021-23-24/vulnerability_repair/major_revision_2021_09/only_first_line_context3_more_parameters_models/${sweep}_parameter_sweep/big_vul_random_fine_tune_test.src.txt.predictions
  echo "Transfer learning"
  python3 compare.py --tgt /cephyr/NOBACKUP/groups/snic2021-23-24/vulnerability_repair/major_revision_2021_09/only_first_line_context3_more_parameters_models/big_vul_random_test_data/big_vul_random_fine_tune_test.tgt.txt --src /cephyr/NOBACKUP/groups/snic2021-23-24/vulnerability_repair/major_revision_2021_09/only_first_line_context3_more_parameters_models/${sweep}_parameter_sweep/big_vul_random_fine_tune/big_vul_random_fine_tune_test.src.txt.predictions
  echo
done
