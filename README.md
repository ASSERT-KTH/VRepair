# VRepair
Vulnerability Repair scripts

## First experiment

For experiments located in `/home/z/zimin/pfs/vulnerabilityRepair_new/Context3_models`

| Sweep number | Learning rate | Hidden size | Pre-train validation token accuracy | Fine-tune validation token accuracy | Pre-train test sequence accuracy | Fine-tune test sequence accuracy |
|--------------|---------------|-------------|-------------------------------------|-------------------------------------|----------------------------------|----------------------------------|
| 0            | 0.001         | 192         | 79.20% (380k/500k)                  | 83.00% (6k/10k)                     | 14.84% (459/3094)                | 9.86% (305/3094)                 |
| 1            | 0.001         | 256         | 79.62% (360k/500k)                  | 82.34% (3k/10k)                     | 15.80% (489/3094)                | 11.96% (370/3094)                |
| 2            | 0.001         | 384         | N/A                                 | N/A                                 | N/A                              | N/A                              |
| 3            | 0.0005        | 192         | 79.22% (320k/500k)                  | 83.04% (5.5k/10k)                   | 15.84% (490/3094)                | 10.73% (332/3094)                |
| 4            | 0.0005        | 256         | 79.87% (360k/500k)                  | 83.5% (3k/10k)                      | 16.26% (503/3094)                | 12.77% (395/3094)                |
| 5            | 0.0005        | 384         | 80.24% (320k/500k)                  | 82.87% (2.5k/10k)                   | 16.58% (513/3094)                | 12.73% (394/3094)                |
| 6            | 0.0002        | 192         | 78.10% (380k/500k)                  | 80.46% (5k/10k)                     | 14.54% (450/3094)                | 10.67% (330/3094)                |
| 7            | 0.0002        | 256         | 79.05% (360k/500k)                  | 82.51% (4.5k/10k)                   | 15.16% (469/3094)                | 11.22% (347/3094)                |
| 8            | 0.0002        | 384         | 80.15% (320k/500k)                  | 83.20% (3.5k/10k)                   | 16.84% (521/3094)                | 12.57% (389/3094)                |

## Second experiment

Experiments at CSU using a model with starting learning rate of x and hidden size of y.

| File                         | Samples (Lines) | Average tokens per sample | 
|------------------------------|-----------------|---------------------------|
| BugFixFine_train_src.txt     | 3177            | 285.1                     |
| BugFixFine_train_tgt.txt     | 3177            |  27.8                     |
| BugFixFine_2019_test_src.txt | 189             | 316.2                     |
| BugFixFine_2019_test_tgt.txt | 189             |  24.1                     |

| Step count    |                           | Beam width 50               |
| Pretrain+Tune | Training token accuracy   | 2019 test sequence accuracy |
|---------------|---------------------------|-----------------------------|
| 300K+0        |   77.67%                  | 20.63% (39 out of 189)      |
| 360K+0        |   78.08%                  | 20.63% (39 out of 189)      |
| 360K+2K       |   83.82%                  | 31.74% (60 out of 189)      |
| 360K+10K      |   91.37%                  | 44.97% (85 out of 189)      |
