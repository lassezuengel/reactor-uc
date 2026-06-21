\page philosohy Philosophy

Learning a new framework for system-design and synthesis is an intimidating process.
This guide explains the core concepts of reactor-uc and is highly recommended for
beginners.

\note If you are not familiar with Lingua Franca (LF), please refer to the [manual](https://www.lf-lang.org/docs/).

## Building

The design of systems with reactor-uc and LF follows a top-down design
approach, where the programmer starts with his design written in LF. The LF
compiler (`lfc`) translates your design into C code. This generated code uses the
reactor-uc runtime functions to execute the program. When you create federated
(distributed) programs, `lfc` generates subfolders for each federate (node) in your
system. The goal of reactor-uc is to enable integration into existing toolchains. 
For example, for projects based on Zephyr, we provide `lfc`-integration through
a custom `west` command. When building an application the programmer interacts
with `west` as usual, and `west` calls `lfc` to generate C code from the LF source
files, finally `west` uses CMake to configure and build the final executable.
Another example is RIOT OS, which has a Make-based toolchain. For RIOT we
integrate `lfc` into the application Makefile such that calling `make all`
first invokes `lfc` on the LF sources before compiling the generated sources.

`lfc` produces importable CMake and make files that you can import into your
build-system. For common platforms like [Zephyr](https://zephyrproject.org/),
[RIOT](https://riot-os.org) or the
[pico-sdk](https://www.raspberrypi.com/documentation/pico-sdk/) we provide
build-templates. 


## Federation Design
We call the network abstraction in reactor-uc `NetworkChannels`, which are bidirectional
message pipes between two federates. Depending on which platform you are working on,
reactor-uc supports different NetworkChannels, such as: `TcpIpChannel`, `UARTChannel` or
`CoapUdpChannel`. You can find a complete table with all network channels that are
supported on the individual platforms [here](TODO).

```lf
federated reactor {
  @interface_tcp(name="if1", address="127.0.0.1")
  src = new Src()

  @interface_tcp(name="if1", address="127.0.0.1")
  dst = new Dst()

  @link(left="if1", right="if1", server_side="right", server_port=1042)
  src.out -> dst.in
}
```

## External Clock Synchronization

Federated programs can delegate clock synchronization to an external implementation with
the `@external_clock_sync(module="...")` attribute. The module must provide an
`ExternalClockSyncApi` through the generated names `lf_clock_sync_init` and
`lf_clock_sync_schedule`.

External synchronization is callback based. The runtime passes a result callback and
opaque user data to `lf_clock_sync_init`; the external implementation stores both and
calls the callback whenever a synchronization run completes. The callback is safe to call
from another cooperative thread or worker queue because it only schedules a system event.
The actual clock offset application happens later in the reactor-uc runtime context.

\code{.c}
static ExternalClockSyncResultCallback result_callback;
static void* result_callback_user_data;

int lf_clock_sync_init(bool grandmaster, int federate_id,
                       ExternalClockSyncResultCallback callback,
                       void* callback_user_data,
                       bool* lf_drives_sync_schedule) {
  result_callback = callback;
  result_callback_user_data = callback_user_data;

  // true: reactor-uc calls lf_clock_sync_schedule at the callback-reported next time.
  // false: this implementation drives its own timing and calls result_callback itself.
  *lf_drives_sync_schedule = true;
  return 0;
}

int lf_clock_sync_schedule(void) {
  int status = 0;
  int64_t next_sync_run_ms = compute_next_sync_time();
  int64_t clock_offset_ms = compute_clock_offset();

  result_callback(result_callback_user_data, status, next_sync_run_ms, clock_offset_ms);
  return 0;
}
\endcode

If `*lf_drives_sync_schedule` is set to `false`, reactor-uc does not schedule calls to
`lf_clock_sync_schedule`. In that mode, the external implementation is responsible for
starting synchronization rounds on its own timeline and reporting each completed round
with the result callback.

