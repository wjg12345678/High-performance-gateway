服务器压力测试
===============
使用 wrk 进行高性能压力测试。wrk 是一个现代化的 HTTP 基准测试工具，能够生成大量并发连接并测试服务器性能。

> * 测试处在相同硬件上，不同服务的性能以及不同硬件上同一个服务的运行状况。
> * 展示服务器的关键指标：吞吐量（QPS）、延迟（平均/最大）、错误率等。

测试规则
------------
* 快速测试示例

    ```bash
    wrk -c 1000 -d 5s -t 4 http://127.0.0.1:9006/
    ```

* 使用脚本测试

    ```bash
    ./wrk_test.sh http://127.0.0.1:9006/ 1000 5s 4
    ```

* 参数说明

> * `-c` 表示并发连接数
> * `-d` 表示测试持续时间（如 5s、1m）
> * `-t` 表示线程数
> * URL 是目标服务器地址

测试结果示例
---------
wrk 对服务器进行压力测试，可以实现上万的并发连接。

输出示例：
```
Running 5s test @ http://127.0.0.1:9006/
  4 threads and 1000 connections
  Thread Stats   Avg      Stdev     Max   +/- Stdev
    Latency   812.88ms  245.32ms   2.50s    65.23%
    Req/Sec    27.35     15.42     89.00     68.92%
  109 requests in 5.00s, 8.45KB read
  Socket errors: connect 0, read 12889, write 0, timeout 25
Requests/sec:    109.42
Transfer/sec:      1.69KB
```

关键指标：
> * Requests/sec：每秒请求数（QPS）
> * Latency：响应延迟（平均值、标准差、最大值）
> * Transfer/sec：每秒传输数据量
> * Socket errors：连接错误统计
