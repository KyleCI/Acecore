<?php

declare(strict_types=1);

/**
 * Synthetic estimate.load_ac_energy.v1 records.
 * These are not hardware observations. simulation is true on every record.
 * Do not post this fixture to a live user database.
 *
 * @return list<array<string, mixed>>
 */
function estimate_demo_records(string $deviceId): array
{
    $model = static function (string $scope = 'inverter_output_only'): array {
        return [
            'revision' => 'estimate.load_ac_energy.v1',
            'source_path' => 'ac_vrms_mv_x_ac_irms_ma_x_pf_millis',
            'version' => '0.1.0',
            'pf_kind' => 'assumed',
            'pf_assumption_millis' => 800,
            'coverage_scope' => $scope,
        ];
    };
    $modes = static function (string $inverter, string $common = '0', string $unknown = '0'): array {
        return [
            'inverter_output_only_ms' => $inverter,
            'common_output_ms' => $common,
            'unknown_ms' => $unknown,
        ];
    };
    $inputs = static function (string $millivolts, string $milliamps): array {
        return [
            'ac_vrms_mv' => $millivolts,
            'ac_irms_ma' => $milliamps,
            'pf_assumed_millis' => 800,
            'ac_v_valid' => true,
            'ac_i_valid' => true,
        ];
    };
    $record = static function (
        string $deviceId,
        string $seq,
        string $startMs,
        string $counterStart,
        string $counterEnd,
        string $utcStart,
        string $utcEnd,
        string $validity,
        string $covered,
        array $model,
        array $modes,
        ?array $inputs,
        string $timeQuality = 'rtc_ds1307'
    ): array {
        $endMs = (string) ((int) $startMs + 3600000);

        return [
            'contract' => 'estimate.load_ac_energy.v1',
            'simulation' => true,
            'device_id' => $deviceId,
            'stream' => 'estimate.load_ac_energy',
            'meter_epoch' => '1',
            'boot_id' => 'sim-boot-1',
            'seq' => $seq,
            'clock_correction_version' => 0,
            'model' => $model,
            'monotonic_start_ms' => $startMs,
            'monotonic_end_ms' => $endMs,
            'utc_start' => $utcStart,
            'utc_end' => $utcEnd,
            'time_quality' => $timeQuality,
            'counter_start_mwh' => $counterStart,
            'counter_end_mwh' => $counterEnd,
            'interval_estimated_mwh' => (string) ((int) $counterEnd - (int) $counterStart),
            'covered_ms' => $covered,
            'interval_ms' => '3600000',
            'sample_validity' => $validity,
            'source_mode_coverage' => $modes,
            'inputs' => $inputs,
        ];
    };

    return [
        $record($deviceId, '1', '0', '0', '0', '2026-11-03T11:00:00Z', '2026-11-03T12:00:00Z', 'valid', '3600000', $model(), $modes('3600000'), $inputs('230000', '0')),
        $record($deviceId, '2', '3600000', '0', '184000', '2026-11-04T11:00:00Z', '2026-11-04T12:00:00Z', 'valid', '3600000', $model(), $modes('3600000'), $inputs('230000', '1000')),
        $record($deviceId, '3', '7200000', '184000', '276000', '2026-11-05T11:00:00Z', '2026-11-05T12:00:00Z', 'partial', '1800000', $model(), $modes('1800000'), $inputs('230000', '1000')),
        $record($deviceId, '4', '10800000', '276000', '644000', '2026-11-06T11:00:00Z', '2026-11-06T12:00:00Z', 'valid', '3600000', $model(), $modes('3600000'), $inputs('230000', '2000')),
        $record($deviceId, '5', '14400000', '644000', '828000', '2026-11-07T11:00:00Z', '2026-11-07T12:00:00Z', 'valid', '3600000', $model(), $modes('3600000'), $inputs('230000', '1000')),
        $record($deviceId, '6', '18000000', '828000', '1012000', '2026-11-07T12:00:00Z', '2026-11-07T13:00:00Z', 'valid', '3600000', $model(), $modes('3600000'), $inputs('230000', '1000')),
        $record($deviceId, '7', '21600000', '1012000', '1196000', '2026-11-08T11:00:00Z', '2026-11-08T12:00:00Z', 'valid', '3600000', $model(), $modes('3600000'), $inputs('230000', '1000')),
        $record($deviceId, '8', '25200000', '1196000', '1380000', '2026-11-08T22:30:00Z', '2026-11-08T23:30:00Z', 'valid', '3600000', $model(), $modes('3600000'), $inputs('230000', '1000')),
        $record($deviceId, '9', '28800000', '1380000', '1564000', '2026-11-09T01:00:00Z', '2026-11-09T02:00:00Z', 'valid', '3600000', $model(), $modes('3600000'), $inputs('230000', '1000'), 'uncertain'),
    ];
}
