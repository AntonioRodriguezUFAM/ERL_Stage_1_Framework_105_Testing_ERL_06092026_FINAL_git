/// Scheduler.md

/*
modelo C++ pronto de um workload scheduler para o Jetson Nano, já no espírito HRL (política estratégica do agente de RL ? atuação tática do scheduler), com DVFS, ajuste de threads, useGPU, e integração simples com seu SystemMetricsAggregator
. 
É um único header para facilitar o drop-in no seu repo.

O que ele faz

Aplica políticas: MAX_PERFORMANCE, BALANCED, LOW_POWER.

Ajusta DVFS: nvpmodel (5W/10W), jetson_clocks, governors de CPU/GPU, min/max freq de CPU/GPU.

Ajusta nível de concorrência (threads) e uso de GPU no seu AlgorithmConcrete via callbacks.

Possui histerese e cooldown para evitar flapping.

Dry-run opcional (log sem tocar no sistema), e graceful fallback com logs quando sem sudo.
*/
 
 https://chatgpt.com/c/69110171-f138-8331-ac05-11d82dc2698b

 Integração com o seu Scheduler (políticas: LOW_POWER / BALANCED / MAX_PERF)

A ideia: seu Agente de RL escolhe a política; o Scheduler aplica ações DVFS correspondentes. Exemplo simples em C++ (escrevendo no sysfs):

Ajustar **DVFS** (frequência e governor de CPU/GPU) no **Jetson Nano** é direto  **não é via CUDA**, e sim via **sysfs** e ferramentas nativas da NVIDIA.
Abaixo estão **os métodos corretos**, desde manual ? programático ? integrado ao seu Scheduler.

---

## 1) **Ativar o modo de desempenho máximo** (desbloquear clocks)

```bash
sudo nvpmodel -m 0
sudo jetson_clocks
```

* `nvpmodel -m 0` ? habilita modo **5W/10W performance** dependendo do Nano
* `jetson_clocks` ? força **todas as frequências no máximo**

**Para reverter:**

```bash
sudo nvpmodel -m 1
sudo jetson_clocks --restore
```

---

## 2) **Controlar a CPU (governor DVFS)**

Listar governors disponíveis:

```bash
cat /sys/devices/system/cpu/cpu0/cpufreq/scaling_available_governors
```

Aplicar governadores:

```bash
# Modo Performance (máx)
echo performance | sudo tee /sys/devices/system/cpu/cpu*/cpufreq/scaling_governor

# Modo Economia
echo powersave | sudo tee /sys/devices/system/cpu/cpu*/cpufreq/scaling_governor

# Modo Adaptativo
echo ondemand | sudo tee /sys/devices/system/cpu/cpu*/cpufreq/scaling_governor
```

Definir frequência fixa (opcional):

```bash
sudo echo <KHz> | sudo tee /sys/devices/system/cpu/cpu0/cpufreq/scaling_max_freq
```

Exemplo:

```bash
echo 921600 | sudo tee /sys/devices/system/cpu/cpu*/cpufreq/scaling_max_freq
```

---

## 3) **Controlar a GPU**

Ver informações atuais:

```bash
sudo cat /sys/devices/gpu.0/devfreq/57000000.gpu/cur_freq
sudo cat /sys/devices/gpu.0/devfreq/57000000.gpu/available_frequencies
```

Ajustar DVFS da GPU:

```bash
# Governor performance
echo performance | sudo tee /sys/devices/gpu.0/devfreq/57000000.gpu/governor

# Governor powersave
echo powersave | sudo tee /sys/devices/gpu.0/devfreq/57000000.gpu/governor
```

Fixar frequência GPU:

```bash
echo <FREQ> | sudo tee /sys/devices/gpu.0/devfreq/57000000.gpu/min_freq
echo <FREQ> | sudo tee /sys/devices/gpu.0/devfreq/57000000.gpu/max_freq
```

---

## 4) **Automatizar no seu Scheduler (C++ dentro da sua arquitetura)**

Crie um **helper**:

```cpp
#include <fstream>

void setSysfsValue(const std::string& path, const std::string& value) {
    std::ofstream ofs(path);
    if (ofs) ofs << value;
}

void setCpuGovernor(const std::string& gov) {
    for (int i = 0; i < 4; i++)
        setSysfsValue("/sys/devices/system/cpu/cpu" + std::to_string(i) + "/cpufreq/scaling_governor", gov);
}

void setGpuGovernor(const std::string& gov) {
    setSysfsValue("/sys/devices/gpu.0/devfreq/57000000.gpu/governor", gov);
}
```

No **Scheduler**, aplique conforme política do agente RL:

```cpp
if (policy == MAX_PERFORMANCE) {
    setCpuGovernor("performance");
    setGpuGovernor("performance");
}
else if (policy == LOW_POWER) {
    setCpuGovernor("powersave");
    setGpuGovernor("powersave");
}
else if (policy == BALANCED) {
    setCpuGovernor("ondemand");
    setGpuGovernor("performance");
}
```

---

## 5) **Integração perfeita com RL**

Sua **Ação Estratégica RL** agora controla **políticas** como:

| Política RL     | CPU Governor  | GPU Governor  | Energia | Desempenho |
| --------------- | ------------- | ------------- | ------- | ---------- |
| MAX_PERFORMANCE | `performance` | `performance` | ?       | ??         |
| BALANCED        | `ondemand`    | `performance` | ?       | ?          |
| LOW_POWER       | `powersave`   | `powersave`   | ?       | ?          |

Isso encaixa **perfeitamente** no modelo HRL que você propôs. ?

---

## 6) **Aviso importante**

> Para executar `DVFS` dinamicamente, seus processos precisam rodar como **root**
> Recomendação: executar seu sistema com `sudo` ou usar `setcap sys_admin`.

---

## Quer que eu agora gere:

**a)** Implementação **completa** do Scheduler em C++ (pronto para compilar)
**b)** Script de calibração automático + teste de FPS/energia
**c)** Slide para apresentação explicando este controle

Responda apenas:

> **a**, **b** ou **c**



perfeito! abaixo vai um **modelo C++ pronto** de um *workload scheduler* para o Jetson Nano, já no espírito HRL (política estratégica do agente de RL ? atuação tática do scheduler), com **DVFS**, ajuste de *threads*, *useGPU*, e integração simples com seu `SystemMetricsAggregator`. É um **único header** para facilitar o drop-in no seu repo.

> **O que ele faz**
>
> * Aplica **políticas**: `MAX_PERFORMANCE`, `BALANCED`, `LOW_POWER`.
> * Ajusta **DVFS**: `nvpmodel` (5W/10W), *jetson_clocks*, *governors* de CPU/GPU, *min/max freq* de CPU/GPU.
> * Ajusta **nível de concorrência** (threads) e **uso de GPU** no seu `AlgorithmConcrete` via *callbacks*.
> * Possui **histerese e cooldown** para evitar *flapping*.
> * **Dry-run** opcional (log sem tocar no sistema), e *graceful fallback* com logs quando sem `sudo`.

---

## `Scheduler.hpp` (coloque em `Scheduler/` ou `Module/`)

```cpp
#pragma once
#include <string>
#include <vector>
#include <functional>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cerrno>
#include <optional>
#include <fstream>
#include <sstream>
#include <thread>

// Se você já usa spdlog no projeto, descomente a linha abaixo e use spdlog.
// #include <spdlog/spdlog.h>
#ifndef SPDLOG_INFO
  #define SPDLOG_INFO(...)  do { fprintf(stderr, "[INFO] "  __VA_ARGS__);  fprintf(stderr, "\n"); } while(0)
  #define SPDLOG_WARN(...)  do { fprintf(stderr, "[WARN] "  __VA_ARGS__);  fprintf(stderr, "\n"); } while(0)
  #define SPDLOG_ERROR(...) do { fprintf(stderr, "[ERROR] " __VA_ARGS__);  fprintf(stderr, "\n"); } while(0)
#endif

// ---- Integração leve com seu agregador ----
// Adapte os campos a seguir aos nomes do seu SystemMetricsSnapshot
struct LiteMetrics {
  double fps = 0.0;
  double latency_ms = 0.0;
  double avgPowerW_algWindow = 0.0;
  double temp_cpu_c = 0.0;
  double temp_gpu_c = 0.0;
  double cpu_util = 0.0;   // 0..100
  double gpu_util = 0.0;   // 0..100
};

// Políticas estratégicas decididas pelo agente de RL
enum class Policy {
  MAX_PERFORMANCE,
  BALANCED,
  LOW_POWER
};

// Parâmetros táticos que o scheduler manda para o módulo de algoritmo
struct AlgoTuning {
  int  concurrencyLevel = 2;
  bool useGPU = true;
};

// Perfil DVFS por política
struct DVFSProfile {
  int nvpmodel_mode = 1;      // Jetson Nano: 0=5W, 1=10W (ajuste conforme seu /etc/nvpmodel.conf)
  bool jetson_clocks = false; // true liga "jetson_clocks" (max clocks). Use com parcimônia.
  std::string cpu_governor = "schedutil"; // performance|schedutil|powersave
  int cpu_min_khz = 0;        // 0 = não alterar
  int cpu_max_khz = 0;        // 0 = não alterar
  std::string gpu_governor = "simple_ondemand"; // depende do devfreq disponível
  int gpu_min_khz = 0;        // 0 = não alterar
  int gpu_max_khz = 0;        // 0 = não alterar
};

// Política completa = DVFS + Tuning de algoritmo
struct PolicyProfile {
  Policy policy;
  DVFSProfile dvfs;
  AlgoTuning  algo;
};

// Executor simples p/ comandos de sistema e sysfs
class SysExec {
public:
  explicit SysExec(bool dryRun) : dryRun_(dryRun) {}

  bool runCmd(const std::string& cmd) const {
    SPDLOG_INFO("[sys] %s%s", dryRun_ ? "(dry) " : "", cmd.c_str());
    if (dryRun_) return true;
    int rc = std::system(cmd.c_str());
    if (rc != 0) SPDLOG_WARN("[sys] rc=%d for cmd: %s", rc, cmd.c_str());
    return rc == 0;
  }

  bool writeFile(const std::string& path, const std::string& value) const {
    SPDLOG_INFO("[sysfs] %s%s = %s", dryRun_ ? "(dry) " : "", path.c_str(), value.c_str());
    if (dryRun_) return true;
    std::ofstream f(path);
    if (!f.good()) {
      SPDLOG_WARN("[sysfs] open failed: %s (%s)", path.c_str(), std::strerror(errno));
      return false;
    }
    f << value;
    bool ok = f.good();
    if (!ok) SPDLOG_WARN("[sysfs] write failed: %s", path.c_str());
    return ok;
  }

private:
  bool dryRun_{false};
};

// Scheduler: converte a Ação estratégica (Policy) em microdecisões (DVFS + tuning)
class WorkloadScheduler {
public:
  struct Config {
    bool dryRun = false;
    // Cooldown para evitar flapping de DVFS
    int cooldown_ms = 1500;
    // Histerese: margens para troca automática de política (opcional)
    double highTempC = 75.0;     // se acima disso, puxar p/ política mais fria
    double lowPowerW = 3.0;      // se consumo médio cair abaixo, pode afrouxar
    double highPowerW = 7.5;     // se consumo subir muito, reduzir
  };

  // Callbacks para integrar com seu módulo de algoritmo em tempo de execução
  using AlgoApplyFn = std::function<void(const AlgoTuning&)>;
  // Se quiser sinalizar a troca de política para log/UI
  using PolicySignalFn = std::function<void(Policy)>;

  explicit WorkloadScheduler(const Config& cfg,
                             AlgoApplyFn algoApply,
                             PolicySignalFn policySignal = {})
  : cfg_(cfg), sys_(cfg.dryRun), algoApply_(std::move(algoApply)), signal_(std::move(policySignal)) {
    // Perfis default (a, b, c)
    profiles_ = {
      PolicyProfile{
        Policy::MAX_PERFORMANCE,
        DVFSProfile{
          /*nvpmodel*/1, /*jetson_clocks*/true,
          /*cpu_gov*/"performance", /*cpu_min*/0, /*cpu_max*/0,
          /*gpu_gov*/"simple_ondemand", /*gpu_min*/0, /*gpu_max*/0
        },
        AlgoTuning{/*conc*/std::thread::hardware_concurrency()>0? (int)std::thread::hardware_concurrency():4, /*useGPU*/true}
      },
      PolicyProfile{
        Policy::BALANCED,
        DVFSProfile{
          1, false,
          "schedutil",  0, 0,
          "simple_ondemand", 0, 0
        },
        AlgoTuning{2, true}
      },
      PolicyProfile{
        Policy::LOW_POWER,
        DVFSProfile{
          0, false,
          "powersave", 0, 0,   // você pode setar min/max freq mais agressivo se quiser
          "simple_ondemand", 0, 0
        },
        AlgoTuning{1, false}
      }
    };
  }

  // Força política (vindas do agente de RL)
  void setPolicy(Policy p) {
    if (p == currentPolicy_) return;
    SPDLOG_INFO("[sched] setPolicy -> %s", policyName(p));
    currentPolicy_ = p;
    lastPolicyChange_ = now();
    applyCurrentProfile_();
    if (signal_) signal_(p);
  }

  Policy getPolicy() const { return currentPolicy_; }

  // Tick periódico com métricas atuais (opcional: auto-ajuste e histerese)
  void tick(const LiteMetrics& m) {
    // Se ainda em cooldown, não mexe
    if (!cooldownExpired_()) return;

    // Exemplo simples de histerese:
    // - Se temp muito alta, cair uma marcha
    if (m.temp_cpu_c > cfg_.highTempC || m.temp_gpu_c > cfg_.highTempC || m.avgPowerW_algWindow > cfg_.highPowerW) {
      if (currentPolicy_ != Policy::LOW_POWER) setPolicy(stepDown_(currentPolicy_));
      return;
    }
    // - Se consumo muito baixo e FPS estável, dá para subir
    if (m.avgPowerW_algWindow < cfg_.lowPowerW && m.fps >= 30.0) {
      if (currentPolicy_ != Policy::MAX_PERFORMANCE) setPolicy(stepUp_(currentPolicy_));
      return;
    }
    // Caso contrário, manter
  }

  // Permite customizar os perfis (a, b, c)
  void setProfile(Policy p, const PolicyProfile& prof) {
    for (auto& x : profiles_) if (x.policy == p) { x = prof; break; }
  }

private:
  static const char* policyName(Policy p) {
    switch(p){
      case Policy::MAX_PERFORMANCE: return "MAX_PERFORMANCE";
      case Policy::BALANCED:        return "BALANCED";
      case Policy::LOW_POWER:       return "LOW_POWER";
    }
    return "UNKNOWN";
  }

  static Policy stepDown_(Policy p) {
    if (p == Policy::MAX_PERFORMANCE) return Policy::BALANCED;
    if (p == Policy::BALANCED)        return Policy::LOW_POWER;
    return Policy::LOW_POWER;
  }
  static Policy stepUp_(Policy p) {
    if (p == Policy::LOW_POWER)       return Policy::BALANCED;
    if (p == Policy::BALANCED)        return Policy::MAX_PERFORMANCE;
    return Policy::MAX_PERFORMANCE;
  }

  using Clock = std::chrono::steady_clock;
  static Clock::time_point now(){ return Clock::now(); }
  bool cooldownExpired_() const {
    auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(now() - lastPolicyChange_).count();
    return ms >= cfg_.cooldown_ms;
  }

  void applyCurrentProfile_() {
    const auto* prof = find_(currentPolicy_);
    if (!prof) { SPDLOG_ERROR("[sched] perfil não encontrado"); return; }

    // 1) Ajustes de DVFS / sistema
    applyDVFS_(*prof);

    // 2) Ajustes no algoritmo (threads, GPU)
    if (algoApply_) algoApply_(prof->algo);
  }

  const PolicyProfile* find_(Policy p) const {
    for (const auto& x : profiles_) if (x.policy == p) return &x;
    return nullptr;
  }

  void applyDVFS_(const PolicyProfile& prof) {
    // nvpmodel
    sys_.runCmd("sudo nvpmodel -m " + std::to_string(prof.dvfs.nvpmodel_mode));

    // jetson_clocks
    if (prof.dvfs.jetson_clocks) {
      sys_.runCmd("sudo jetson_clocks");
    } else {
      // Opcional: restaurar clocks (se você previamente fez --store)
      // sys_.runCmd("sudo jetson_clocks --restore");
    }

    // CPU governor
    setCpuGovernor_(prof.dvfs.cpu_governor);

    // CPU min/max (se fornecidos)
    if (prof.dvfs.cpu_min_khz > 0 || prof.dvfs.cpu_max_khz > 0) {
      setCpuFreqRange_(prof.dvfs.cpu_min_khz, prof.dvfs.cpu_max_khz);
    }

    // GPU governor
    setGpuGovernor_(prof.dvfs.gpu_governor);

    // GPU min/max
    if (prof.dvfs.gpu_min_khz > 0 || prof.dvfs.gpu_max_khz > 0) {
      setGpuFreqRange_(prof.dvfs.gpu_min_khz, prof.dvfs.gpu_max_khz);
    }
  }

  // ---- Helpers específicos do Nano (ajuste para sua placa se necessário) ----
  // CPUs: /sys/devices/system/cpu/cpuX/cpufreq/{scaling_governor,scaling_min_freq,scaling_max_freq}
  void setCpuGovernor_(const std::string& gov) {
    int cores = detectCpuCores_();
    for (int i=0;i<cores;i++) {
      sys_.writeFile("/sys/devices/system/cpu/cpu"+std::to_string(i)+"/cpufreq/scaling_governor", gov+"\n");
    }
  }

  void setCpuFreqRange_(int min_khz, int max_khz) {
    int cores = detectCpuCores_();
    for (int i=0;i<cores;i++) {
      if (min_khz>0) sys_.writeFile("/sys/devices/system/cpu/cpu"+std::to_string(i)+"/cpufreq/scaling_min_freq", std::to_string(min_khz)+"\n");
      if (max_khz>0) sys_.writeFile("/sys/devices/system/cpu/cpu"+std::to_string(i)+"/cpufreq/scaling_max_freq", std::to_string(max_khz)+"\n");
    }
  }

  // GPU devfreq no Nano costuma ser 57000000.gpu (confirme com `ls /sys/devices/57000000.gpu/devfreq/57000000.gpu/`)
  void setGpuGovernor_(const std::string& gov) {
    sys_.writeFile("/sys/devices/57000000.gpu/devfreq/57000000.gpu/governor", gov+"\n");
  }
  void setGpuFreqRange_(int min_khz, int max_khz) {
    if (min_khz>0) sys_.writeFile("/sys/devices/57000000.gpu/devfreq/57000000.gpu/min_freq", std::to_string(min_khz)+"\n");
    if (max_khz>0) sys_.writeFile("/sys/devices/57000000.gpu/devfreq/57000000.gpu/max_freq", std::to_string(max_khz)+"\n");
  }

  int detectCpuCores_() const {
    // fallback simples; pode ler /proc/cpuinfo
    unsigned c = std::thread::hardware_concurrency();
    return c>0 ? (int)c : 4;
  }

private:
  Config cfg_;
  SysExec sys_;
  AlgoApplyFn   algoApply_;
  PolicySignalFn signal_;
  std::vector<PolicyProfile> profiles_;

  Policy currentPolicy_{Policy::BALANCED};
  std::chrono::steady_clock::time_point lastPolicyChange_{std::chrono::steady_clock::now()};
};
```

---

## Como plugar no seu pipeline

### 1) Crie o scheduler

```cpp
#include "Scheduler.hpp"

// Callback que aplica tuning no seu AlgorithmConcrete em tempo de execução
auto applyAlgo = [&](const AlgoTuning& t){
  // Exemplo: chame métodos thread-safe do seu AlgorithmConcrete
  // algo->setConcurrencyLevel(t.concurrencyLevel);
  // algo->setUseGPU(t.useGPU);
  SPDLOG_INFO("[algo] conc=%d useGPU=%d", t.concurrencyLevel, (int)t.useGPU);
};

// (Opcional) sinal para UI/logs
auto onPolicy = [&](Policy p){ SPDLOG_INFO("[policy] %d", (int)p); };

WorkloadScheduler::Config cfg;
cfg.dryRun = false;     // true para testar sem root
cfg.cooldown_ms = 1500;

WorkloadScheduler scheduler(cfg, applyAlgo, onPolicy);
```

### 2) Agente de RL ? Scheduler

Quando seu agente decidir a estratégia:

```cpp
scheduler.setPolicy(Policy::MAX_PERFORMANCE); // ou BALANCED / LOW_POWER
```

### 3) Loop periódico com métricas (auto-histerese opcional)

Alimente com um snapshot reduzido (mapeie do seu `SystemMetricsSnapshot`):

```cpp
LiteMetrics m;
m.fps      = snapshot.fps_effective_display;
m.latency_ms = snapshot.latency_ms_alg;
m.avgPowerW_algWindow = snapshot.avgPowerW_algWindow;
m.temp_cpu_c = snapshot.temp_cpu_c;
m.temp_gpu_c = snapshot.temp_gpu_c;
m.cpu_util   = snapshot.cpu_util_avg_algWindow;
m.gpu_util   = snapshot.gpu_util_avg_algWindow;

scheduler.tick(m); // pode manter política atual ou ajustar 1 passo
```

---

## Observações importantes (Jetson Nano / DVFS)

* **Permissões:** escrita em `/sys` e `nvpmodel/jetson_clocks` requer `sudo`. Execute seu binário com os devidos privilégios ou crie regras `udev/sudoers`.
* **nvpmodel (Nano):** perfis comuns: `-m 0` (**5W**) e `-m 1` (**10W**). Confirme com `sudo nvpmodel -q --verbose`.
* **jetson_clocks:** fixa clocks no máximo (CPU/GPU/EMC). Útil para `MAX_PERFORMANCE`. Para voltar, use `--restore` se você previamente fez `--store`.
* **GPU devfreq:** o caminho `57000000.gpu` é típico no Nano; confirme no seu sistema (`ls /sys/devices/*gpu*/devfreq/*/`).
* **GoVernors:** `performance`, `schedutil`, `powersave` (CPU). Para GPU, geralmente `simple_ondemand`; valores válidos podem ser vistos em `.../available_governors`.

---

## Ajustes a, b e c (perfis prontos)

* **a) MAX_PERFORMANCE**
  `nvpmodel=10W`, `jetson_clocks=ON`, CPU `performance`, GPU *ondemand*, `concurrencyLevel = #cores`, `useGPU = true`.

* **b) BALANCED**
  `nvpmodel=10W`, `jetson_clocks=OFF`, CPU `schedutil`, GPU *ondemand*, `concurrencyLevel = 2`, `useGPU = true`.

* **c) LOW_POWER**
  `nvpmodel=5W`, `jetson_clocks=OFF`, CPU `powersave`, (opcional: reduzir `cpu_max_khz`), `concurrencyLevel = 1`, `useGPU = false`.

Você pode customizar frequências mín/max por política com:

```cpp
PolicyProfile p = ...;
p.dvfs.cpu_min_khz = 307200; // exemplo
p.dvfs.cpu_max_khz = 1200000;
p.dvfs.gpu_min_khz = 76800;
p.dvfs.gpu_max_khz = 921600;
scheduler.setProfile(Policy::LOW_POWER, p);
```

---

Se quiser, eu adapto esse header para integrar diretamente ao seu `ConfigManager` (carregar perfis do `config.json`) e gerar *patches* no formato `git apply`. Também posso incluir um driver de DVFS para **EMC (memória)** se você quiser controlar banda/latência de memória no Nano.
