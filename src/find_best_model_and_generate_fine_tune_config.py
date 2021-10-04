import argparse
import yaml
from pathlib import Path

def get_opennmt_fine_tune_config(opennmt_pre_train_config, best_model_path,
                                 save_model_path_pattern, tensorboard_log_dir,
                                 train_features_file, train_labels_file,
                                 valid_features_file, valid_labels_file,
                                 train_steps=10000, valid_steps=100,
                                 save_checkpoint_steps=100,
                                 start_decay_steps=500, decay_steps=500,
                                 learning_rate_decay=0.8):
    opennmt_pre_train_config['train_from'] = best_model_path
    opennmt_pre_train_config['reset_optim'] = 'all'
    opennmt_pre_train_config['save_model'] = save_model_path_pattern

    opennmt_pre_train_config['data']['github']['path_src'] = train_features_file
    opennmt_pre_train_config['data']['github']['path_tgt'] = train_labels_file
    opennmt_pre_train_config['data']['valid']['path_src'] = valid_features_file
    opennmt_pre_train_config['data']['valid']['path_tgt'] = valid_labels_file

    opennmt_pre_train_config['learning_rate'] = opennmt_pre_train_config['learning_rate'] / 10
    opennmt_pre_train_config['learning_rate_decay'] = learning_rate_decay

    opennmt_pre_train_config['train_steps'] = train_steps
    opennmt_pre_train_config['valid_steps'] = valid_steps
    opennmt_pre_train_config['save_checkpoint_steps'] = save_checkpoint_steps
    opennmt_pre_train_config['start_decay_steps'] = start_decay_steps
    opennmt_pre_train_config['decay_steps'] = decay_steps
    opennmt_pre_train_config['early_stopping'] = 4
    opennmt_pre_train_config['accum_count'] = 2
    opennmt_pre_train_config['keep_checkpoint'] = 10

    opennmt_pre_train_config['tensorboard'] = True
    opennmt_pre_train_config['tensorboard_log_dir'] = tensorboard_log_dir

    return opennmt_pre_train_config


def default_hpc2n_job_script(opennmt_fine_tune_config_path, gpu_type='k80',
                             number_of_gpus='1', time='6:00:00'):
    log_file_path = Path(opennmt_fine_tune_config_path).parent / 'log.txt'
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
onmt_train --config {opennmt_fine_tune_config_path} 2>&1 | tee -a {log_file_path}

    '''.format(
        gpu_type=gpu_type, number_of_gpus=number_of_gpus, time=time,
        opennmt_fine_tune_config_path=opennmt_fine_tune_config_path,
        log_file_path=str(log_file_path).replace('\\', '\\\\')
    )
    return hpc2n_job_script


def default_c3se_job_script(opennmt_fine_tune_config_path, gpu_type='T4',
                            number_of_gpus='1', time='24:00:00'):
    log_file_path = Path(opennmt_fine_tune_config_path).parent / 'log.txt'
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
onmt_train --config {opennmt_fine_tune_config_path} 2>&1 | tee -a {log_file_path}

    '''.format(
        gpu_type=gpu_type, number_of_gpus=number_of_gpus, time=time,
        opennmt_fine_tune_config_path=opennmt_fine_tune_config_path,
        log_file_path=str(log_file_path).replace('\\', '\\\\')
    )
    return c3se_job_script


def main():
    parser = argparse.ArgumentParser(
        description='Automatic creating data.yml for the fine tuning process.')
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
    parser.add_argument('-fine_tune_dirname', action="store", dest='fine_tune_dirname',
                        help="Directory name for fine tuning.")
    args = parser.parse_args()

    train_features_file = Path(args.train_features_file).resolve()
    train_labels_file = Path(args.train_labels_file).resolve()
    eval_features_file = Path(args.eval_features_file).resolve()
    eval_labels_file = Path(args.eval_labels_file).resolve()
    sweep_root_path = Path(args.sweep_root_path).resolve()
    c3se_cluster = args.c3se_cluster
    if args.fine_tune_dirname:
        fine_tune_dirname = args.fine_tune_dirname
    else:
        fine_tune_dirname = 'fine_tune'

    for file in [train_features_file, train_labels_file, eval_features_file, eval_labels_file, sweep_root_path]:
        assert(file.exists())

    for sweep_path in sweep_root_path.rglob('*_parameter_sweep'):
        all_model_checkpoints = list(sweep_path.rglob('*.pt'))
        all_model_checkpoints.sort(key=lambda x: int(x.stem.split('_')[-1]))
        best_model_step = -1
        with open(sweep_path / 'log.txt') as log_file:
            for line in log_file:
                if 'Best model found at step' in line:
                    best_model_step = line.strip().split(' ')[-1]
                    break
        if best_model_step == -1:
            best_model_path = all_model_checkpoints[-1]
        else:
            best_model_path = sweep_path / ('model_step_' + best_model_step + '.pt')

        with open(sweep_path / 'train_config.yml') as f:
            opennmt_pre_train_config = yaml.safe_load(f)

        fine_tune_dir = sweep_path / fine_tune_dirname
        fine_tune_dir.mkdir(parents=True, exist_ok=True)
        save_model_path_pattern = fine_tune_dir / 'model'
        tensorboard_log_dir = fine_tune_dir / 'tensorboard_log_dir'
        opennmt_fine_tune_config = get_opennmt_fine_tune_config(
            opennmt_pre_train_config,
            str(best_model_path).replace('\\', '\\\\'),
            str(save_model_path_pattern).replace('\\', '\\\\'),
            str(tensorboard_log_dir).replace('\\', '\\\\'),
            str(train_features_file).replace('\\', '\\\\'),
            str(train_labels_file).replace('\\', '\\\\'),
            str(eval_features_file).replace('\\', '\\\\'),
            str(eval_labels_file).replace('\\', '\\\\'))

        fine_tune_config_path = fine_tune_dir / 'fine_tune_config.yml'
        with open(fine_tune_config_path, mode='w', encoding='utf-8') as f:
            yaml.dump(opennmt_fine_tune_config, f, default_flow_style=False,
                      allow_unicode=True, sort_keys=False)
        if c3se_cluster:
            job_script = default_c3se_job_script(
                str(fine_tune_config_path).replace('\\', '\\\\'))
        else:
            job_script = default_hpc2n_job_script(
                str(fine_tune_config_path).replace('\\', '\\\\'))
        job_script_path = fine_tune_dir / 'job.sh'
        with open(job_script_path, mode='w', encoding='utf8') as f:
            f.write(job_script)


if __name__ == "__main__":
    main()
