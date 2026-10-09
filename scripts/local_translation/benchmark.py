"""Measure the live service, cold/cache latency, and total-capacity CPU usage."""
from __future__ import annotations

import argparse
import http.client
import json
from pathlib import Path
import statistics
import time

EXAMPLES = [
    ("我明天上午去图书馆。", "ZH", "EN"),
    ("这个翻译模型完全在本地运行，不需要联网。", "ZH", "EN"),
    ("请把这个文件保存到桌面，然后重新启动程序。", "ZH", "EN"),
    ("如果明天下雨，我们就在家里看电影。", "ZH", "EN"),
    ("I will go to the library tomorrow morning.", "EN", "ZH"),
    ("Please save this file on the desktop and restart the application.", "EN", "ZH"),
]


def request(config, path, body=None, timeout=2.5):
    connection = http.client.HTTPConnection("127.0.0.1", config["port"], timeout=timeout)
    headers = {"Authorization": "Bearer " + config["token"], "Content-Type": "application/json"}
    try:
        encoded = json.dumps(body, ensure_ascii=False).encode("utf-8") if body is not None else None
        started = time.perf_counter()
        connection.request("POST" if body is not None else "GET", path, encoded, headers)
        response = connection.getresponse()
        result = json.loads(response.read())
        elapsed = time.perf_counter() - started
        if response.status != 200:
            raise RuntimeError(f"Service returned HTTP {response.status}: {result.get('message', '')}")
        return result, elapsed
    finally:
        connection.close()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--config", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--stress-seconds", type=float, default=20)
    args = parser.parse_args()
    import psutil  # Optional benchmark dependency, not part of the service.

    config = json.loads(args.config.read_text(encoding="utf-8"))
    health, _ = request(config, "/health")
    process = psutil.Process(health["pid"])
    records = []
    for text, source, target in EXAMPLES:
        body = {"text": text, "source_lang": source, "target_lang": target}
        for repeat in range(2):
            result, elapsed = request(config, "/translate", body)
            records.append({"text": text, "translation": result["data"], "cached": result["cached"],
                            "latency_ms": round(elapsed * 1000, 3)})
    cpu_start = sum(process.cpu_times()[:2])
    wall_start = time.perf_counter()
    count = 0
    while time.perf_counter() - wall_start < args.stress_seconds:
        request(config, "/translate", {"text": f"请在明天下午处理第{count + 1}份测试文件。",
                                       "source_lang": "ZH", "target_lang": "EN"})
        count += 1
    stress_wall = time.perf_counter() - wall_start
    stress_cpu = sum(process.cpu_times()[:2]) - cpu_start
    idle_cpu = sum(process.cpu_times()[:2])
    idle_start = time.perf_counter()
    time.sleep(3)
    idle_wall = time.perf_counter() - idle_start
    idle_cpu = sum(process.cpu_times()[:2]) - idle_cpu
    fresh = [r["latency_ms"] for r in records if not r["cached"]]
    cached = [r["latency_ms"] for r in records if r["cached"]]
    report = {"health": health, "examples": records,
              "fresh_max_ms": max(fresh, default=None), "fresh_median_ms": statistics.median(fresh) if fresh else None,
              "cached_max_ms": max(cached, default=None), "stress_requests": count,
              "stress_wall_seconds": stress_wall, "stress_cpu_seconds": stress_cpu,
              "stress_cpu_total_percent": stress_cpu / stress_wall / health["logical_processors"] * 100,
              "idle_cpu_total_percent": idle_cpu / idle_wall / health["logical_processors"] * 100,
              "rss_mib": process.memory_info().rss / 1024**2,
              "cpu_percent_basis": "Process CPU seconds / wall seconds / logical processors * 100"}
    args.output.write_text(json.dumps(report, ensure_ascii=False, indent=2) + "\n", encoding="utf-8")
    print(json.dumps({k: v for k, v in report.items() if k not in ("examples",)}, ensure_ascii=False))


if __name__ == "__main__":
    main()
