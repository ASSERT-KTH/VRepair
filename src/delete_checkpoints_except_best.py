import argparse
from pathlib import Path

parser = argparse.ArgumentParser(
    description='Delete all model checkpoints except for the best one.')
parser.add_argument('-sweep_root_path', action="store", dest='sweep_root_path',
                    help="Path to the root directory of all configs sweeps")
args = parser.parse_args()

def main():
    sweep_root_path = Path(args.sweep_root_path).resolve()
    for log_file in sweep_root_path.rglob('log.txt'):
        log_dir = log_file.parent
        all_model_checkpoints = list(log_dir.glob('*.pt'))
        all_model_checkpoints.sort(key=lambda x: int(x.stem.split('_')[-1]))
        best_model_step = -1
        with open(log_file) as f:
            for line in f:
                if 'Best model found at step' in line:
                    best_model_step = line.strip().split(' ')[-1]
                    break
        if best_model_step == -1:
            best_model_path = all_model_checkpoints[-1]
        else:
            best_model_path = log_dir / ('model_step_' + best_model_step + '.pt')

        assert best_model_path in all_model_checkpoints

        for checkpoint_file in log_dir.glob('*.pt'):
            if checkpoint_file != best_model_path:
                checkpoint_file.unlink()

if __name__ == "__main__":
    main()
