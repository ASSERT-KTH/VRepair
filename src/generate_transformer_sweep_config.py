import re
import io
import yaml
import argparse
import itertools
import collections
from pathlib import Path
yaml.add_representer(collections.defaultdict,
                     yaml.representer.Representer.represent_dict)


def nested_dict(): return collections.defaultdict(nested_dict)


def get_opennmt_train_config(save_data_path_pattern, save_model_path_pattern,
                             tensorboard_log_dir,
                             train_features_file, train_labels_file,
                             valid_features_file, valid_labels_file,
                             train_steps=1000000, valid_steps=20000,
                             src_vocab_size=2000, tgt_vocab_size=2000,
                             src_seq_length=1000, tgt_seq_length=100,
                             number_of_gpus=1, batch_size=4,
                             valid_batch_size=1, optim='adam',
                             learning_rate=0.0005, learning_rate_decay=0.9,
                             label_smoothing=0.1,
                             enc_layers=6, dec_layers=6, heads=8, rnn_size=256,
                             word_vec_size=256, transformer_ff=2048,
                             dropout=0.1, attention_dropout=0.1,
                             accum_count=8, early_stopping=2,
                             seed=0):
    opennmt_train_config = nested_dict()

    opennmt_train_config['world_size'] = number_of_gpus
    opennmt_train_config['gpu_ranks'] = list(range(number_of_gpus))

    opennmt_train_config['batch_size'] = batch_size
    opennmt_train_config['valid_batch_size'] = valid_batch_size

    opennmt_train_config['src_vocab'] = save_data_path_pattern + '.vocab.src'
    opennmt_train_config['tgt_vocab'] = save_data_path_pattern + '.vocab.tgt'
    opennmt_train_config['src_vocab_size'] = src_vocab_size
    opennmt_train_config['tgt_vocab_size'] = tgt_vocab_size
    opennmt_train_config['src_seq_length'] = src_seq_length
    opennmt_train_config['tgt_seq_length'] = tgt_seq_length

    opennmt_train_config['data']['github']['path_src'] = train_features_file
    opennmt_train_config['data']['github']['path_tgt'] = train_labels_file
    opennmt_train_config['data']['valid']['path_src'] = valid_features_file
    opennmt_train_config['data']['valid']['path_tgt'] = valid_labels_file

    opennmt_train_config['save_model'] = save_model_path_pattern
    opennmt_train_config['overwrite'] = False
    opennmt_train_config['train_steps'] = train_steps
    opennmt_train_config['valid_steps'] = valid_steps
    opennmt_train_config['save_checkpoint_steps'] = valid_steps
    opennmt_train_config['early_stopping'] = early_stopping
    opennmt_train_config['early_stopping_criteria'] = 'accuracy'
    opennmt_train_config['keep_checkpoint'] = 10

    opennmt_train_config['optim'] = optim
    opennmt_train_config['learning_rate'] = learning_rate
    opennmt_train_config['learning_rate_decay'] = learning_rate_decay
    opennmt_train_config['label_smoothing'] = label_smoothing
    opennmt_train_config['param_init'] = 0
    opennmt_train_config['param_init_glorot'] = True

    opennmt_train_config['encoder_type'] = 'transformer'
    opennmt_train_config['decoder_type'] = 'transformer'
    opennmt_train_config['enc_layers'] = enc_layers
    opennmt_train_config['dec_layers'] = dec_layers
    opennmt_train_config['heads'] = heads
    opennmt_train_config['rnn_size'] = rnn_size
    opennmt_train_config['word_vec_size'] = word_vec_size
    opennmt_train_config['transformer_ff'] = transformer_ff
    opennmt_train_config['dropout'] = [dropout]
    opennmt_train_config['attention_dropout'] = [attention_dropout]
    opennmt_train_config['copy_attn'] = True
    opennmt_train_config['position_encoding'] = True
    opennmt_train_config['accum_count'] = accum_count
    opennmt_train_config['bridge'] = True

    opennmt_train_config['tensorboard'] = True
    opennmt_train_config['tensorboard_log_dir'] = tensorboard_log_dir

    opennmt_train_config['seed'] = seed

    return opennmt_train_config


def get_opennmt_vocab_config(save_data_path_pattern,
                             train_features_file, train_labels_file,
                             valid_features_file, valid_labels_file,
                             src_seq_length=1000, tgt_seq_length=100,
                             src_vocab_size=2000, tgt_vocab_size=2000,
                             seed=0):
    opennmt_vocab_config = nested_dict()

    opennmt_vocab_config['save_data'] = save_data_path_pattern
    opennmt_vocab_config['overwrite'] = False
    opennmt_vocab_config['n_sample'] = -1
    opennmt_vocab_config['src_vocab_size'] = src_vocab_size
    opennmt_vocab_config['tgt_vocab_size'] = tgt_vocab_size
    opennmt_vocab_config['src_seq_length'] = src_seq_length
    opennmt_vocab_config['tgt_seq_length'] = tgt_seq_length

    opennmt_vocab_config['data']['github']['path_src'] = train_features_file
    opennmt_vocab_config['data']['github']['path_tgt'] = train_labels_file
    opennmt_vocab_config['data']['valid']['path_src'] = valid_features_file
    opennmt_vocab_config['data']['valid']['path_tgt'] = valid_labels_file

    opennmt_vocab_config['seed'] = seed

    return opennmt_vocab_config


def default_hpc2n_job_script(opennmt_vocab_config_path,
                             opennmt_train_config_path, gpu_type='k80',
                             number_of_gpus='1', time='96:00:00'):
    CWE_vocab_list = ['CWE-119', 'CWE-125', 'CWE-20', 'CWE-200', 'CWE-264',
                      'CWE-476', 'CWE-399', 'CWE-189', 'CWE-416', 'CWE-190',
                      'CWE-362', 'CWE-787', 'CWE-284', 'CWE-772', 'CWE-415']
    insert_vocab = '\\n'.join(
        [CWE_id + '\\t99999999' for CWE_id in CWE_vocab_list])
    src_vocab_file_path = Path(
        opennmt_vocab_config_path).parent / 'data.vocab.src'
    log_file_path = Path(opennmt_vocab_config_path).parent / 'log.txt'
    hpc2n_job_script = '''\
#!/bin/bash

# Project to run under
#SBATCH -A SNIC2020-5-453
# Name of the job (makes easier to find in the status lists)
#SBATCH -J repair
# Exclusive use when using more than 2 GPUs
#SBATCH --exclusive
# Use GPU
#SBATCH --gres=gpu:{gpu_type}:{number_of_gpus}
# the job can use up to x minutes to run
#SBATCH --time={time}

# run the program
onmt_build_vocab -config {opennmt_vocab_config_path}
sed -i '1i{insert_vocab}' {src_vocab_file_path}
onmt_train --config {opennmt_train_config_path} 2>&1 | tee -a {log_file_path}

    '''.format(
        gpu_type=gpu_type, number_of_gpus=number_of_gpus, time=time,
        opennmt_vocab_config_path=opennmt_vocab_config_path,
        insert_vocab=insert_vocab,
        src_vocab_file_path=str(src_vocab_file_path).replace('\\', '\\\\'),
        opennmt_train_config_path=opennmt_train_config_path,
        log_file_path=str(log_file_path).replace('\\', '\\\\')
    )
    return hpc2n_job_script


def default_c3se_job_script(opennmt_vocab_config_path,
                            opennmt_train_config_path, gpu_type='T4',
                            number_of_gpus='1', time='144:00:00'):
    CWE_vocab_list = ['CWE-119', 'CWE-125', 'CWE-20', 'CWE-200', 'CWE-264',
                      'CWE-476', 'CWE-399', 'CWE-189', 'CWE-416', 'CWE-190',
                      'CWE-362', 'CWE-787', 'CWE-284', 'CWE-772', 'CWE-415']
    insert_vocab = '\\n'.join(
        [CWE_id + '\\t99999999' for CWE_id in CWE_vocab_list])
    src_vocab_file_path = Path(
        opennmt_vocab_config_path).parent / 'data.vocab.src'
    log_file_path = Path(opennmt_vocab_config_path).parent / 'log.txt'
    c3se_job_script = '''\
#!/bin/bash

# Project to run under
#SBATCH -A SNIC2020-33-63 -p alvis
# Name of the job (makes easier to find in the status lists)
#SBATCH -J repair
# Use GPU
#SBATCH --gpus-per-node={gpu_type}:{number_of_gpus}
# the job can use up to x minutes to run
#SBATCH --time={time}

# run the program
onmt_build_vocab -config {opennmt_vocab_config_path}
sed -i '1i{insert_vocab}' {src_vocab_file_path}
onmt_train --config {opennmt_train_config_path} 2>&1 | tee -a {log_file_path}

    '''.format(
        gpu_type=gpu_type, number_of_gpus=number_of_gpus, time=time,
        opennmt_vocab_config_path=opennmt_vocab_config_path,
        insert_vocab=insert_vocab,
        src_vocab_file_path=str(src_vocab_file_path).replace('\\', '\\\\'),
        opennmt_train_config_path=opennmt_train_config_path,
        log_file_path=str(log_file_path).replace('\\', '\\\\')
    )
    return c3se_job_script


def update_learning_rate(config, learning_rate):
    config['learning_rate'] = learning_rate
    return config


def update_train_batch_size(config, batch_size):
    config['batch_size'] = batch_size
    return config


def update_embedding_size_and_num_units(config, size):
    config['rnn_size'] = size
    config['word_vec_size'] = size
    return config


def update_num_enc_layers(config, num_layers):
    config['enc_layers'] = num_layers
    return config


def update_num_dec_layers(config, num_layers):
    config['dec_layers'] = num_layers
    return config


def update_num_layers(config, num_layers):
    config['enc_layers'] = num_layers
    config['dec_layers'] = num_layers
    return config


def update_num_heads(config, num_heads):
    config['heads'] = num_heads
    return config


def update_ffn_inner_dim(config, ffn_inner_dim):
    config['transformer_ff'] = ffn_inner_dim
    return config


def update_vocab_size(config, vocab_size):
    config['src_vocab_size'] = vocab_size
    config['tgt_vocab_size'] = vocab_size
    return config


def update_dropout(config, dropout_prob):
    config['dropout'] = dropout_prob
    config['attention_dropout'] = dropout_prob
    return config


def main():
    parser = argparse.ArgumentParser(
        description='Automatic creating data.yml for OpenNMT-tf and copy all data.')
    parser.add_argument('-train_features_file', action="store",
                        dest='train_features_file', help="Path to train_features_file")
    parser.add_argument('-train_labels_file', action="store",
                        dest='train_labels_file', help="Path to train_labels_file")
    parser.add_argument('-eval_features_file', action="store",
                        dest='eval_features_file', help="Path to eval_features_file")
    parser.add_argument('-eval_labels_file', action="store",
                        dest='eval_labels_file', help="Path to eval_labels_file")
    parser.add_argument('-sweep_root_path', action="store", dest='sweep_root_path',
                        help="Path to the root directory of all configs sweeps")
    parser.add_argument('-c3se_cluster', action="store_true", dest='c3se_cluster',
                        help="Generate job script for C3SE cluster, otherwise for "
                        "the HPC2N cluster. Default HPC2N cluster.")
    args = parser.parse_args()

    train_features_file = Path(args.train_features_file).resolve()
    train_labels_file = Path(args.train_labels_file).resolve()
    eval_features_file = Path(args.eval_features_file).resolve()
    eval_labels_file = Path(args.eval_labels_file).resolve()
    sweep_root_path = Path(args.sweep_root_path).resolve()
    c3se_cluster = args.c3se_cluster

    for file in [train_features_file, train_labels_file, eval_features_file, eval_labels_file, sweep_root_path]:
        assert(file.exists())

    learning_rate_sweep = list((update_learning_rate, learning_rate)
                               for learning_rate in [0.0005, 0.0001, 0.00005])
    hidden_size_sweep = list((update_embedding_size_and_num_units, hidden_size)
                             for hidden_size in [256, 512, 1024])
    vocab_size_sweep = list((update_vocab_size, vocab_size)
                            for vocab_size in [2000, 5000, 10000])

    parameter_sweep = [learning_rate_sweep,
                       hidden_size_sweep, vocab_size_sweep]
    for index, updates in enumerate(itertools.product(*parameter_sweep)):
        sweep_path = sweep_root_path / (str(index) + '_parameter_sweep')
        sweep_path.mkdir(parents=True, exist_ok=True)

        save_data_path_pattern = sweep_path / 'data'
        save_model_path_pattern = sweep_path / 'model'
        tensorboard_log_dir = sweep_path / 'tensorboard_log_dir'
        opennmt_vocab_config = get_opennmt_vocab_config(
            str(save_data_path_pattern).replace('\\', '\\\\'),
            str(train_features_file).replace('\\', '\\\\'),
            str(train_labels_file).replace('\\', '\\\\'),
            str(eval_features_file).replace('\\', '\\\\'),
            str(eval_labels_file).replace('\\', '\\\\'))
        opennmt_train_config = get_opennmt_train_config(
            str(save_data_path_pattern).replace('\\', '\\\\'),
            str(save_model_path_pattern).replace('\\', '\\\\'),
            str(tensorboard_log_dir).replace('\\', '\\\\'),
            str(train_features_file).replace('\\', '\\\\'),
            str(train_labels_file).replace('\\', '\\\\'),
            str(eval_features_file).replace('\\', '\\\\'),
            str(eval_labels_file).replace('\\', '\\\\'))
        for update_func, parameter in updates:
            opennmt_vocab_config = update_func(opennmt_vocab_config, parameter)
        for update_func, parameter in updates:
            opennmt_train_config = update_func(opennmt_train_config, parameter)

        opennmt_vocab_config_path = sweep_path / 'vocab_config.yml'
        with io.open(opennmt_vocab_config_path, 'w', encoding='utf8') as f:
            yaml.dump(opennmt_vocab_config, f, default_flow_style=False,
                      allow_unicode=True, sort_keys=False)
        opennmt_train_config_path = sweep_path / 'train_config.yml'
        with io.open(opennmt_train_config_path, 'w', encoding='utf8') as f:
            yaml.dump(opennmt_train_config, f, default_flow_style=False,
                      allow_unicode=True, sort_keys=False)
        if c3se_cluster:
            job_script = default_c3se_job_script(
                str(opennmt_vocab_config_path).replace('\\', '\\\\'),
                str(opennmt_train_config_path).replace('\\', '\\\\'))
        else:
            job_script = default_hpc2n_job_script(
                str(opennmt_vocab_config_path).replace('\\', '\\\\'),
                str(opennmt_train_config_path).replace('\\', '\\\\'))
        job_script_path = sweep_path / 'job.sh'
        with io.open(job_script_path, 'w', encoding='utf8') as f:
            f.write(job_script)


if __name__ == "__main__":
    main()
