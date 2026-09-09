---

### 3. `USER_GUIDE.md` (Configuration)

This helps your team run experiments.

```markdown
# User Guide & Configuration

## config.json
The system behavior is defined in `config.json`. Here are the critical sections for ERL experiments.

### 1. Defining Constraints
This section tells the ERL Agent what "Success" looks like.
```json
"SystemBehavior": {
    "targetFPS": 30,
    "maxPowerWatts": 5.0,
    "maxTempCelsius": 65.0,
    "prioritizeBalance": true
}