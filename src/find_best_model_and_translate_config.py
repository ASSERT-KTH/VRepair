import argparse
import yaml
from pathlib import Path


def default_hpc2n_translate_script(best_model_path, test_src_path, output_path,
                                   beam_size=50, gpu_type='k80',
                                   number_of_gpus='1', time='2:00:00'):
    hpc2n_translate_script = '''\
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
onmt_translate -model {best_model_path} -src {test_src_path} -output {output_path} -beam_size {beam_size} -n_best {beam_size} -batch_size 1 -gpu {gpu_ids}

    '''.format(
        gpu_type=gpu_type, number_of_gpus=number_of_gpus, time=time,
        best_model_path=best_model_path, test_src_path=test_src_path,
        output_path=output_path, beam_size=beam_size,
        gpu_ids=','.join([str(i) for i in range(int(number_of_gpus))])
    )
    return hpc2n_translate_script


def default_c3se_translate_script(best_model_path, test_src_path, output_path,
                                  beam_size=50, gpu_type='T4',
                                  number_of_gpus='1', time='2:00:00'):
    c3se_translate_script = '''\
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
onmt_translate -model {best_model_path} -src {test_src_path} -output {output_path} -beam_size {beam_size} -n_best {beam_size} -batch_size 1 -gpu {gpu_ids}

    '''.format(
        gpu_type=gpu_type, number_of_gpus=number_of_gpus, time=time,
        best_model_path=best_model_path, test_src_path=test_src_path,
        output_path=output_path, beam_size=beam_size,
        gpu_ids=','.join([str(i) for i in range(int(number_of_gpus))])
    )
    return c3se_translate_script


def main():
    parser = argparse.ArgumentParser(
        description='Automatic creating data.yml for the fine tuning process.')
    parser.add_argument('-test_features_file', action="store",
                        dest='test_features_file', help="Path to train_features_file")
    parser.add_argument('-pre_train_model', action="store_true", dest='pre_train_model',
                        help="Generate translate script for the pre-trained models, "
                        "otherwise for fine tune models. Default fine tune models.")
    parser.add_argument('-sweep_root_path', action="store", dest='sweep_root_path',
                        help="Path to the root directory of all configs sweeps")
    parser.add_argument('-c3se_cluster', action="store_true", dest='c3se_cluster',
                        help="Generate job script for C3SE cluster, otherwise for "
                        "the HPC2N cluster. Default HPC2N cluster.")
    parser.add_argument('-fine_tune_dirname', action="store", dest='fine_tune_dirname',
                        help="Directory name for fine tuning.")
    parser.add_argument('-pre_train_jobname', action="store", dest='pre_train_jobname',
                        help="Job name for the pre training prediction.")
    args = parser.parse_args()

    test_features_file = Path(args.test_features_file).resolve()
    pre_train_model = args.pre_train_model
    sweep_root_path = Path(args.sweep_root_path).resolve()
    c3se_cluster = args.c3se_cluster
    if args.fine_tune_dirname:
        fine_tune_dirname = args.fine_tune_dirname
    else:
        fine_tune_dirname = 'fine_tune'

    for file in [test_features_file, sweep_root_path]:
        assert(file.exists())


    for sweep_path in sweep_root_path.rglob('*_parameter_sweep'):
        if pre_train_model:
            models_path = sweep_path
        else:
            models_path = sweep_path / fine_tune_dirname
        all_model_checkpoints = list(models_path.rglob('*.pt'))
        all_model_checkpoints.sort(key=lambda x: int(x.stem.split('_')[-1]))
        best_model_step = -1
        with open(models_path / 'log.txt') as log_file:
            for line in log_file:
                if 'Best model found at step' in line:
                    best_model_step = line.strip().split(' ')[-1]
                    break
        if best_model_step == -1:
            best_model_path = all_model_checkpoints[-1]
        else:
            best_model_path = models_path / ('model_step_' + best_model_step + '.pt')

        output_path = models_path / (test_features_file.name + '.predictions')

        if c3se_cluster:
            translate_script = default_c3se_translate_script(
                str(best_model_path).replace('\\', '\\\\'),
                str(test_features_file).replace('\\', '\\\\'),
                str(output_path).replace('\\', '\\\\'))
        else:
            translate_script = default_hpc2n_translate_script(
                str(best_model_path).replace('\\', '\\\\'),
                str(test_features_file).replace('\\', '\\\\'),
                str(output_path).replace('\\', '\\\\'))

        translate_script_path = models_path / 'translate.sh'
        if pre_train_model and args.pre_train_jobname:
            translate_script_path = models_path / args.pre_train_jobname
        with open(translate_script_path, mode='w', encoding='utf8') as f:
            f.write(translate_script)


if __name__ == "__main__":
    main()
