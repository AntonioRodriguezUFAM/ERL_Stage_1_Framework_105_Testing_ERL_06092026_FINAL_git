"""
metrics_analyzer.py
===================
Production-grade metrics analysis engine for ERL Framework
- Parses lynsyn (power rails) and realtime_metrics (system telemetry)
- Calculates derived metrics (power, energy, efficiency)
- Detects anomalies and validates data integrity
- Generates statistical summaries for PhD documentation
"""

import pandas as pd
import numpy as np
from datetime import datetime
from typing import Dict, List, Tuple, Optional
import logging
from dataclasses import dataclass
from enum import Enum

# Configure logging
logging.basicConfig(
    level=logging.INFO,
    format='[%(asctime)s] %(levelname)s: %(message)s'
)
logger = logging.getLogger(__name__)


class PowerRail(Enum):
    """Jetson Nano power rails (from Lynsyn)"""
    RAIL_0 = ("i0", "v0", "CPU0")      # CPU cluster 0
    RAIL_1 = ("i1", "v1", "CPU1")      # CPU cluster 1
    RAIL_2 = ("i2", "v2", "SoC")       # System-on-Chip
    RAIL_3 = ("i3", "v3", "GPU")       # GPU domain
    RAIL_4 = ("i4", "v4", "MEMORY")    # DDR memory
    RAIL_5 = ("i5", "v5", "IO")        # I/O domain
    RAIL_6 = ("i6", "v6", "AUX")       # Auxiliary


@dataclass
class PowerSample:
    """Single power measurement across all rails"""
    timestamp: float
    rail_0_power: float  # CPU0 (W)
    rail_1_power: float  # CPU1 (W)
    rail_2_power: float  # SoC (W)
    rail_3_power: float  # GPU (W)
    rail_4_power: float  # Memory (W)
    rail_5_power: float  # I/O (W)
    rail_6_power: float  # Aux (W)
    
    @property
    def total_power(self) -> float:
        """Sum of all rails"""
        return (self.rail_0_power + self.rail_1_power + self.rail_2_power +
                self.rail_3_power + self.rail_4_power + self.rail_5_power +
                self.rail_6_power)


@dataclass
class FrameMetrics:
    """Per-frame metrics aggregated from system measurements"""
    frame_id: int
    timestamp: float
    fps: float
    algo_inference_ms: float
    algo_total_proc_ms: float
    display_latency_ms: float
    end_to_end_latency_ms: float
    joules_per_frame: float
    cpu_util_avg: float
    gpu_util_avg: float
    cpu_temp_c: float
    gpu_temp_c: float
    dropped_frames: int


class CSVParser:
    """Parse Lynsyn and realtime_metrics CSVs with validation"""
    
    @staticmethod
    def parse_lynsyn(csv_path: str) -> pd.DataFrame:
        """
        Parse Lynsyn power rail measurements.
        
        Columns: time_s, pc0-pc3, i0-i6, v0-v6
        Returns dataframe with calculated power per rail
        """
        logger.info(f"Loading Lynsyn data from {csv_path}")
        df = pd.read_csv(csv_path)
        
        # Validate required columns
        required_cols = ['time_s'] + [f'i{i}' for i in range(7)] + [f'v{i}' for i in range(7)]
        missing = [col for col in required_cols if col not in df.columns]
        if missing:
            raise ValueError(f"Missing columns in Lynsyn CSV: {missing}")
        
        # Calculate power per rail (P = V × I)
        for i in range(7):
            df[f'p{i}'] = df[f'v{i}'] * df[f'i{i}']
        
        # Normalize timestamps (start at 0)
        df['time_s'] = df['time_s'] - df['time_s'].min()
        
        logger.info(f"Loaded {len(df)} power samples, duration {df['time_s'].max():.2f}s")
        return df
    
    @staticmethod
    def parse_realtime_metrics(csv_path: str) -> pd.DataFrame:
        """
        Parse realtime system metrics.
        
        Returns dataframe with normalized timestamps
        """
        logger.info(f"Loading realtime metrics from {csv_path}")
        df = pd.read_csv(csv_path)
        
        # Parse timestamp (format: "2026-04-12 17:39:54.222")
        if 'Timestamp' in df.columns:
            df['datetime'] = pd.to_datetime(df['Timestamp'])
            df['time_s'] = (df['datetime'] - df['datetime'].min()).dt.total_seconds()
        
        logger.info(f"Loaded {len(df)} frame metrics, duration {df['time_s'].max():.2f}s")
        return df


class MetricsCalculator:
    """Calculate derived metrics and statistics"""
    
    @staticmethod
    def calculate_power_stats(lynsyn_df: pd.DataFrame) -> Dict[str, Dict[str, float]]:
        """
        Calculate statistics for each power rail.
        
        Returns dict: rail_name -> {mean, std, min, max, p50, p95, p99}
        """
        stats = {}
        
        for i in range(7):
            rail_name = PowerRail(f"RAIL_{i}").value[2]
            power_col = f'p{i}'
            
            power_data = lynsyn_df[power_col]
            stats[rail_name] = {
                'mean_w': power_data.mean(),
                'std_w': power_data.std(),
                'min_w': power_data.min(),
                'max_w': power_data.max(),
                'p50_w': power_data.quantile(0.50),
                'p95_w': power_data.quantile(0.95),
                'p99_w': power_data.quantile(0.99),
                'samples': len(power_data)
            }
        
        # Total power stats
        total_power = sum([lynsyn_df[f'p{i}'] for i in range(7)])
        stats['TOTAL'] = {
            'mean_w': total_power.mean(),
            'std_w': total_power.std(),
            'min_w': total_power.min(),
            'max_w': total_power.max(),
            'p50_w': total_power.quantile(0.50),
            'p95_w': total_power.quantile(0.95),
            'p99_w': total_power.quantile(0.99),
            'samples': len(total_power)
        }
        
        return stats
    
    @staticmethod
    def calculate_energy_efficiency(lynsyn_df: pd.DataFrame, 
                                   metrics_df: pd.DataFrame) -> Dict[str, float]:
        """
        Validate and summarize energy efficiency.
        
        Compares measured joules/frame from system vs. derived from power rails
        """
        # Calculate total energy from power rails (integral)
        total_power = sum([lynsyn_df[f'p{i}'] for i in range(7)])
        dt = np.mean(np.diff(lynsyn_df['time_s']))  # sample interval
        total_energy_j = (total_power * dt).sum()
        
        # Count frames processed
        processed_frames = metrics_df[metrics_df['FrameID'] > 0].shape[0]
        
        if processed_frames > 0:
            energy_per_frame_derived = total_energy_j / processed_frames
            energy_per_frame_reported = metrics_df['JoulesPerFrame'].mean()
            
            return {
                'total_energy_j': total_energy_j,
                'frames_processed': processed_frames,
                'energy_per_frame_derived_mj': energy_per_frame_derived * 1000,
                'energy_per_frame_reported_mj': energy_per_frame_reported * 1000,
                'validation_error_pct': abs(
                    energy_per_frame_derived - energy_per_frame_reported
                ) / max(energy_per_frame_reported, 1e-6) * 100
            }
        
        return {}
    
    @staticmethod
    def calculate_thermal_stats(metrics_df: pd.DataFrame) -> Dict[str, Dict[str, float]]:
        """Calculate CPU and GPU temperature statistics"""
        stats = {
            'CPU': {
                'mean_c': metrics_df['SoC_CPU_Util'].mean() if 'SoC_CPU_Util' in metrics_df else 0,
                'max_c': metrics_df['SoC_CPU_Util'].max() if 'SoC_CPU_Util' in metrics_df else 0,
                'std_c': metrics_df['SoC_CPU_Util'].std() if 'SoC_CPU_Util' in metrics_df else 0,
            }
        }
        return stats
    
    @staticmethod
    def calculate_latency_breakdown(metrics_df: pd.DataFrame) -> Dict[str, float]:
        """Analyze latency components"""
        valid_frames = metrics_df[metrics_df['FrameID'] > 0]
        
        return {
            'algo_inference_mean_ms': valid_frames['AlgoInferenceMs'].mean(),
            'algo_inference_std_ms': valid_frames['AlgoInferenceMs'].std(),
            'algo_inference_p95_ms': valid_frames['AlgoInferenceMs'].quantile(0.95),
            'display_latency_mean_ms': valid_frames['DisplayLatencyMs'].mean(),
            'end_to_end_latency_mean_ms': valid_frames['EndToEndLatencyMs'].mean(),
            'end_to_end_latency_p95_ms': valid_frames['EndToEndLatencyMs'].quantile(0.95),
            'dropped_frames_total': valid_frames['AlgoDroppedFrames'].sum(),
        }


class AnomalyDetector:
    """Detect data quality issues"""
    
    @staticmethod
    def detect_power_anomalies(lynsyn_df: pd.DataFrame, 
                               threshold_std: float = 3.0) -> Dict[str, List[int]]:
        """Identify power spikes using Z-score"""
        anomalies = {}
        
        for i in range(7):
            power_col = f'p{i}'
            power_data = lynsyn_df[power_col]
            
            mean = power_data.mean()
            std = power_data.std()
            z_scores = np.abs((power_data - mean) / std)
            
            anomaly_indices = np.where(z_scores > threshold_std)[0].tolist()
            if anomaly_indices:
                anomalies[f'Rail_{i}'] = anomaly_indices
        
        return anomalies
    
    @staticmethod
    def detect_timestamp_gaps(df: pd.DataFrame, 
                             expected_interval_ms: float = 10.0) -> List[Tuple[float, float]]:
        """Find gaps in time series"""
        time_diffs = np.diff(df['time_s']) * 1000  # Convert to ms
        expected = expected_interval_ms
        gaps = []
        
        for i, diff in enumerate(time_diffs):
            if diff > expected * 2:  # Gap > 2x expected
                gaps.append((df['time_s'].iloc[i], df['time_s'].iloc[i+1]))
        
        return gaps
    
    @staticmethod
    def detect_missing_data(df: pd.DataFrame) -> Dict[str, int]:
        """Count nulls per column"""
        return df.isnull().sum().to_dict()


class MetricsFramework:
    """Main analysis orchestrator"""
    
    def __init__(self, lynsyn_path: str, metrics_path: str):
        self.lynsyn_path = lynsyn_path
        self.metrics_path = metrics_path
        self.lynsyn_df = None
        self.metrics_df = None
        
    def load_data(self):
        """Load and parse both data sources"""
        self.lynsyn_df = CSVParser.parse_lynsyn(self.lynsyn_path)
        self.metrics_df = CSVParser.parse_realtime_metrics(self.metrics_path)
        logger.info("Data loaded successfully")
    
    def analyze(self) -> Dict:
        """Run full analysis suite"""
        if self.lynsyn_df is None or self.metrics_df is None:
            raise ValueError("Call load_data() first")
        
        logger.info("Starting analysis...")
        
        results = {
            'power_stats': MetricsCalculator.calculate_power_stats(self.lynsyn_df),
            'energy_efficiency': MetricsCalculator.calculate_energy_efficiency(
                self.lynsyn_df, self.metrics_df),
            'thermal_stats': MetricsCalculator.calculate_thermal_stats(self.metrics_df),
            'latency_stats': MetricsCalculator.calculate_latency_breakdown(self.metrics_df),
            'power_anomalies': AnomalyDetector.detect_power_anomalies(self.lynsyn_df),
            'timestamp_gaps': AnomalyDetector.detect_timestamp_gaps(self.lynsyn_df),
            'missing_data': AnomalyDetector.detect_missing_data(self.lynsyn_df),
        }
        
        logger.info("Analysis complete")
        return results
    
    def summary_report(self, results: Dict) -> str:
        """Generate human-readable summary"""
        report = []
        report.append("=" * 80)
        report.append("ERL FRAMEWORK METRICS ANALYSIS REPORT")
        report.append("=" * 80)
        
        # Power Summary
        report.append("\n### POWER CONSUMPTION ###")
        power_stats = results['power_stats']
        report.append(f"{'Rail':<15} {'Mean (W)':<12} {'Std (W)':<12} {'P95 (W)':<12}")
        report.append("-" * 51)
        for rail, stats in power_stats.items():
            report.append(
                f"{rail:<15} {stats['mean_w']:<12.3f} {stats['std_w']:<12.3f} "
                f"{stats['p95_w']:<12.3f}"
            )
        
        # Energy Efficiency
        report.append("\n### ENERGY EFFICIENCY ###")
        eff = results['energy_efficiency']
        if eff:
            report.append(f"Total Energy: {eff['total_energy_j']:.2f} J")
            report.append(f"Frames Processed: {eff['frames_processed']}")
            report.append(f"Energy/Frame (derived): {eff['energy_per_frame_derived_mj']:.2f} mJ")
            report.append(f"Energy/Frame (reported): {eff['energy_per_frame_reported_mj']:.2f} mJ")
            report.append(f"Validation Error: {eff['validation_error_pct']:.1f}%")
        
        # Latency
        report.append("\n### LATENCY ANALYSIS ###")
        lat = results['latency_stats']
        report.append(f"Algo Inference: {lat['algo_inference_mean_ms']:.2f} ± {lat['algo_inference_std_ms']:.2f} ms")
        report.append(f"End-to-End (P95): {lat['end_to_end_latency_p95_ms']:.2f} ms")
        report.append(f"Dropped Frames: {lat['dropped_frames_total']}")
        
        # Data Quality
        report.append("\n### DATA QUALITY ###")
        report.append(f"Power Anomalies Detected: {len(results['power_anomalies'])}")
        report.append(f"Timestamp Gaps: {len(results['timestamp_gaps'])}")
        
        report.append("\n" + "=" * 80)
        
        return "\n".join(report)


if __name__ == "__main__":
    # Example usage
    framework = MetricsFramework(
        "lynsyn_output.csv",
        "realtime_metrics_010.csv"
    )
    framework.load_data()
    results = framework.analyze()
    print(framework.summary_report(results))