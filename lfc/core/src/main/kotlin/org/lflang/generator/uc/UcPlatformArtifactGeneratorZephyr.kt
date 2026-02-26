package org.lflang.generator.uc

import java.nio.file.Files
import java.nio.file.Path
import kotlin.collections.buildList
import kotlin.math.max
import org.lflang.generator.ZephyrConfig
import org.lflang.lf.Instantiation
import org.lflang.target.TargetConfig
import org.lflang.target.property.FedNetInterfaceProperty
import org.lflang.target.property.LoggingProperty
import org.lflang.target.property.PlatformProperty
import org.lflang.target.property.type.FedNetInterfaceType.FedNetInterface
import org.lflang.target.property.type.LoggingType.LogLevel
import org.lflang.util.FileUtil

/**
 * Emits Zephyr-specific build scaffolding. The provided platform context distinguishes between
 * standalone applications and federated executables, allowing future federated-only extensions.
 *
 * Specifically, this generator generates...
 * - a `CMakeLists.txt` that sets up the Zephyr build environment and includes the generated code
 *   from the reactor-uc library. Ready to build!
 * - a `prj_lf.conf` with the necessary Zephyr configuration options for the generated code,
 *   depending on whether the application is standalone or federated. Depending on the target board,
 *   additional board-specific configuration may be included as well (e.g., for Raspberry Pi Pico
 *   boards). Furthermore, if the user has specified additional relevant files in the workspace,
 *   these are added to the build as well.
 */
class UcPlatformArtifactGeneratorZephyr(
    mainDef: Instantiation,
    targetConfig: TargetConfig,
    projectRoot: Path,
    workspaceRoot: Path,
    context: UcGeneratorFactory.PlatformContext
) : UcPlatformArtifactGenerator(mainDef, targetConfig, projectRoot, workspaceRoot, context) {

  private val S = '$'
  private val boardConfigProvider = ZephyrBoardConfigProvider()

  companion object {
    private var fedId = 0
  }

  override fun generate() {
    val platformOptions = targetConfig.get(PlatformProperty.INSTANCE)
    val boardFromTarget = platformOptions.board().value()
    // TODO: The "@board" attribute was added as a quick and dirty hack for convenience and should
    // be removed in favor of a more robust solution for specifying different targets for different
    // federates.
    val boardFromFederate =
        (context as? UcGeneratorFactory.PlatformContext.Federated)?.federate?.board
    val selectedBoard = boardFromFederate ?: boardFromTarget
    val defaultBoard =
        selectedBoard?.takeIf { boardFromFederate != null || platformOptions.board().setByUser() }
            ?: "nrf52840dk_nrf52840"
    val cmake =
        generateCmake(
            init =
                """
            |set(PLATFORM "ZEPHYR" CACHE STRING "Platform to target")
            |if (NOT DEFINED BOARD)
            |  set(BOARD "${defaultBoard}")
            |endif()
            |# Include default lf conf-file.
            |set(CONF_FILE prj_lf.conf)
            |# Include user-provided conf-file, if it exists
            |if(EXISTS prj.conf)
            |  set(OVERLAY_CONFIG prj.conf)
            |endif()
            |set(_LF_ZEPHYR_HINTS)
            |if(DEFINED ENV{ZEPHYR_BASE} AND EXISTS "${S}ENV{ZEPHYR_BASE}")
            |  list(APPEND _LF_ZEPHYR_HINTS "${S}ENV{ZEPHYR_BASE}")
            |endif()
            |if (EXISTS "${'$'}{CMAKE_CURRENT_SOURCE_DIR}/deps/zephyr")
            |  list(APPEND _LF_ZEPHYR_HINTS "${'$'}{CMAKE_CURRENT_SOURCE_DIR}/deps/zephyr")
            |endif()
            |if(_LF_ZEPHYR_HINTS)
            |  find_package(Zephyr REQUIRED HINTS ${'$'}{_LF_ZEPHYR_HINTS})
            |else()
            |  find_package(Zephyr REQUIRED)
            |endif()
            |zephyr_compile_options(-Wno-error=unused-parameter -Wno-error=type-limits)
            |zephyr_compile_definitions(_GNU_SOURCE)
        """
                    .trimMargin(),
            projectName = projectName(),
            mainTargetName = "app",
            createMainTarget = false,
            additionalVariables = additionalVariables())
    FileUtil.writeToFile(cmake, projectRoot.resolve("CMakeLists.txt"))

    val basePrjConf =
        when (context) {
          is UcGeneratorFactory.PlatformContext.Standalone -> generatePrjConfStandalone()
          is UcGeneratorFactory.PlatformContext.Federated -> generatePrjConfFederated(context)
        }

    val boardName = selectedBoard?.lowercase()
    val boardSpecificConfig = boardConfigProvider.configFor(boardName)
    val prjConf =
        listOf(basePrjConf, boardSpecificConfig).filterNotNull().joinToString(separator = "\n\n")

    FileUtil.writeToFile(prjConf, projectRoot.resolve("prj_lf.conf"))

    val mergedKconfig = mergeKconfigWithWorkspace(generateKconfig())
    FileUtil.writeToFile(mergedKconfig, projectRoot.resolve("Kconfig"))

    // Copy user-provided files from the workspace, if they exist. This allows users to provide
    // custom Zephyr configurations without needing to modify the generated code (e.g., by providing
    // a `prj.conf` overlay).
    listOf("prj.conf", "app.overlay").forEach(::copyFromWorkspace)
  }

  /**
   * Generate a minimal prj.conf for standalone applications.
   *
   * TODO: Implement! We need only basic functionality, perhaps printk, no networking stuff.
   */
  private fun generatePrjConfStandalone(): String =
      """
        |CONFIG_ETH_NATIVE_POSIX=n
        |CONFIG_NET_DRIVERS=y
        |CONFIG_NETWORKING=y
        |CONFIG_NET_TCP=y
        |CONFIG_NET_UDP=y
        |CONFIG_NET_IPV4=y
        |CONFIG_NET_SOCKETS=y
        |CONFIG_POSIX_API=y
        |CONFIG_MAIN_STACK_SIZE=16384
        |CONFIG_HEAP_MEM_POOL_SIZE=1024
        |CONFIG_LF_TCP_IP_CHANNEL_STACK_SIZE=4096
        |CONFIG_LF_TCP_IP_CHANNEL_THREAD_PREEMPT_LEVEL=0
        |CONFIG_LF_TCP_IP_CHANNEL_THREAD_NAME="lf_tcpip_rx"
      """
          .trimMargin()

  /**
   * Generate a `prj.conf` for federated applications, enabling necessary Zephyr features for
   * communication and configuration. The generated config includes settings for POSIX sockets,
   * network buffers, IP address configuration, and additional system settings.
   */
  private fun generatePrjConfFederated(fed: UcGeneratorFactory.PlatformContext.Federated): String {
    val netInterface = targetConfig.get(FedNetInterfaceProperty.INSTANCE)
    val useIpv6 = netInterface == FedNetInterface.SICSLOWPAN

    if (!useIpv6) {
      return """
        |CONFIG_ETH_NATIVE_POSIX=n
        |CONFIG_NET_DRIVERS=y
        |CONFIG_NETWORKING=y
        |CONFIG_NET_TCP=y
        |CONFIG_NET_IPV4=y
        |CONFIG_NET_SOCKETS=y
        |CONFIG_POSIX_API=y
        |CONFIG_MAIN_STACK_SIZE=16384
        |CONFIG_HEAP_MEM_POOL_SIZE=1024
        |CONFIG_LF_TCP_IP_CHANNEL_STACK_SIZE=4096
        |CONFIG_LF_TCP_IP_CHANNEL_THREAD_PREEMPT_LEVEL=0
        |CONFIG_LF_TCP_IP_CHANNEL_THREAD_NAME="lf_tcpip_rx"
        |
        |# Network address config
        |CONFIG_NET_CONFIG_SETTINGS=y
        |CONFIG_NET_CONFIG_NEED_IPV4=y
        |CONFIG_NET_CONFIG_MY_IPV4_ADDR="127.0.0.1"
        |CONFIG_NET_SOCKETS_OFFLOAD=y
        |CONFIG_NET_NATIVE_OFFLOADED_SOCKETS=y
      """
          .trimMargin()
    }

    val devIpv6 =
        fed.federate.interfaces
            .asSequence()
            .filterIsInstance<UcTcpIpInterface>()
            .map { it.getIpAddress().address }
            .firstOrNull() ?: "fd01::${fedId++}"

    // TODO: note that this minimum is too small! Check how many connections we need!
    // val axConnections = maxConnectionsFor(fed).toString()
    // TODO: hardcoded for now!
    val maxConnections = "10"

    return ZephyrConfig()
        .comment("Lingua Franca Zephyr configuration file")
        .comment("This is a generated file, do not edit.")
        .blank()
        .property("PRINTK", "y")
        .property("USE_SEGGER_RTT", "y")
        .property("LOG_BACKEND_RTT", "y")
        .property("DEBUG_INFO", "y")
        .property("RTT_CONSOLE", "y")
        .property("UART_CONSOLE", "n")
        .property("LOG_MODE_IMMEDIATE", "y")
        .property("LOG", "y")
        .heading("Diagnostics and logging")
        .property("LOG_PROCESS_THREAD", "n")
        .heading("POSIX sockets and networking")
        .property("NETWORKING", "y")
        .property("NET_IPV6", "y")
        .property("NET_TCP", "y")
        .property("NET_SOCKETS", "y")
        .property("NET_CONNECTION_MANAGER", "y")
        .property("POSIX_API", "y")
        .heading("Network buffers")
        .property("NET_PKT_RX_COUNT", "8")
        .property("NET_PKT_TX_COUNT", "8")
        .property("NET_BUF_RX_COUNT", "16")
        .property("NET_BUF_TX_COUNT", "16")
        .property("NET_CONTEXT_NET_PKT_POOL", "n")
        .heading("IP address options")
        .property("NET_IF_UNICAST_IPV6_ADDR_COUNT", "3")
        .property("NET_IF_MCAST_IPV6_ADDR_COUNT", "4")
        .property("NET_MAX_CONTEXTS", "6")
        .property("NET_MAX_CONN", maxConnections)
        .heading("Network shell")
        .property("NET_SHELL", "n")
        .property("SHELL", "n")
        .heading("Network application options and configs")
        .property("NET_CONFIG_SETTINGS", "y")
        .property("NET_CONFIG_NEED_IPV4", "n")
        .property("NET_CONFIG_NEED_IPV6", "y")
        .property("NET_CONFIG_MY_IPV6_ADDR", "\"$devIpv6\"")
        // .property_if(devIpv6 == "fd01::2", "NET_CONFIG_PEER_IPV6_ADDR", "\"fd01::1\"")
        .property("NET_MAX_CONN", maxConnections)
        .property("ZVFS_OPEN_MAX", "16")
        .property("NET_IF_MAX_IPV6_COUNT", "2")
        .heading("IEEE802.15.4 6LoWPAN")
        .property("BT", "n")
        .property("NET_UDP", "y")
        .property("NET_IPV4", "n")
        .property("NET_L2_IEEE802154_FRAGMENT_REASS_CACHE_SIZE", "8")
        .property("NET_L2_IEEE802154_RADIO_CSMA_CA", "y")
        .property("NET_L2_IEEE802154_RADIO_ALOHA", "n")
        .property("NET_CONFIG_MY_IPV4_ADDR", "\"\"")
        .property("NET_CONFIG_PEER_IPV4_ADDR", "\"\"")
        .property("NET_L2_IEEE802154", "y")
        .property("NET_L2_IEEE802154_SHELL", "n")
        .property("NET_IPV6_ND", "n")
        .property("NET_IPV6_NBR_CACHE", "n")
        .property("NET_CONFIG_IEEE802154_CHANNEL", "26")
        .heading("Additional system configuration")
        .property("SYSTEM_WORKQUEUE_STACK_SIZE", "2048")
        .property("MAIN_STACK_SIZE", "4096")
        .property("LF_TCP_IP_CHANNEL_STACK_SIZE", "2048")
        .property("HEAP_MEM_POOL_SIZE", "1024")
        .property("THREAD_CUSTOM_DATA", "y")
        .comment("Enable floating point formatting/logging support.")
        .comment("This increases code size, so feel free to disable if not needed.")
        .property("CBPRINTF_FP_SUPPORT", "y")
        .generateOutput()
  }

  /**
   * Determine the default Zephyr log level based on the target configuration's logging property.
   * This maps the LF log levels to Zephyr's numeric log levels, allowing us to set an appropriate
   * default log level in the generated configs.
   */
  private fun zephyrLogDefaultLevel(): String {
    return when (targetConfig.getOrDefault(LoggingProperty.INSTANCE)) {
      LogLevel.ERROR -> "1"
      LogLevel.WARN -> "2"
      LogLevel.INFO -> "3"
      LogLevel.LOG -> "3"
      LogLevel.DEBUG -> "4"
      null -> "3" // Default to INFO if not set (reasonable default)
    }
  }

  /**
   * Generate the content of the Kconfig file, which defines configuration options for the Zephyr
   * build. This includes options for the TCP/IP channel worker thread, allowing users to customize
   * its stack size, guard region, preempt priority, and thread name.
   *
   * Because not only this Kconfig, but also the associated prj.conf file is generated and not
   * user-configurable, this is of little use to users in terms of customization, but it does allow
   * us to provide a clear place for us to pass additional compile-time configuration options to the
   * Zephyr build system.
   */
  private fun generateKconfig(): String =
      """
        |source "Kconfig.zephyr"
        |
        |menu "Lingua Franca settings"
        |
        |config LF_TCP_IP_CHANNEL_STACK_SIZE
        |  int "TCP/IP channel worker stack size"
        |  default 4096
        |  help
        |    Stack size in bytes allocated for the Lingua Franca TCP/IP receive worker thread.
        |
        |config LF_TCP_IP_CHANNEL_STACK_GUARD
        |  int "TCP/IP channel worker POSIX guard"
        |  default 128
        |  help
        |    Guard region size (bytes) reserved when the TCP/IP worker uses the POSIX pthread implementation.
        |
        |config LF_TCP_IP_CHANNEL_THREAD_PREEMPT_LEVEL
        |  int "TCP/IP channel worker preempt priority"
        |  default 0
        |  help
        |    Preemptible priority level passed to K_PRIO_PREEMPT() for the TCP/IP worker thread.
        |
        |config LF_TCP_IP_CHANNEL_THREAD_NAME
        |  string "TCP/IP channel worker thread name"
        |  default "lf_tcpip_rx"
        |  help
        |    Optional Zephyr thread name used for kernel tracing and debug output.
        |
        |endmenu
      """
          .trimMargin()

  private fun projectName(): String =
      when (val ctx = context) {
        is UcGeneratorFactory.PlatformContext.Federated -> "${mainDef.name}_${ctx.federate.name}"
        UcGeneratorFactory.PlatformContext.Standalone -> mainDef.name
      }

  /** Generate additional CMake variables based on the target configuration and platform context. */
  private fun additionalVariables(): List<String> = buildList {
    val logLevel =
        "set(LOG_LEVEL LF_LOG_LEVEL_${targetConfig.getOrDefault(LoggingProperty.INSTANCE).name.uppercase()})"
    add(logLevel)
    if (context is UcGeneratorFactory.PlatformContext.Federated) {
      add("set(FEDERATE ${context.federate.name})")
    }
  }

  private fun maxConnectionsFor(fed: UcGeneratorFactory.PlatformContext.Federated): Int =
      max(1, UcConnectionGenerator.getNumNetworkBundles(fed.federate))

  private fun generateCmake(
      init: String,
      projectName: String,
      mainTargetName: String,
      createMainTarget: Boolean,
      additionalVariables: List<String>
  ): String {
    val builder = StringBuilder()
    builder.appendLine("cmake_minimum_required(VERSION 3.20.0)")
    builder.appendLine(init)
    builder.appendLine("project(${projectName})")
    builder.appendLine("set(LF_MAIN ${mainDef.name})")
    builder.appendLine("set(LF_MAIN_TARGET ${mainTargetName})")
    builder.appendLine("set(PROJECT_ROOT ${projectRoot}/..)")
    builder.appendLine("set(KCONFIG_ROOT ${'$'}{CMAKE_CURRENT_SOURCE_DIR}/Kconfig)")
    additionalVariables.forEach { builder.appendLine(it) }
    builder.appendLine()
    if (createMainTarget) {
      builder.appendLine("add_executable(${S}{LF_MAIN_TARGET})")
    }
    builder.appendLine("include(${S}ENV{REACTOR_UC_PATH}/cmake/lfc.cmake)")
    builder.appendLine("lf_setup()")
    builder.appendLine(
        "lf_build_generated_code(${S}{LF_MAIN_TARGET} ${S}{CMAKE_CURRENT_SOURCE_DIR})")
    builder.appendLine("if (NOT TARGET ${S}{LF_MAIN_TARGET})")
    builder.appendLine(
        "  message(FATAL_ERROR \"Target ${S}{LF_MAIN_TARGET} was not created by Zephyr\")")
    builder.appendLine("endif()")
    builder.appendLine()
    return builder.toString()
  }

  /**
   * Merge the generated Kconfig content with any user-provided Kconfig overlay from the workspace.
   * If the user has provided a Kconfig file in the workspace, its content will be appended to the
   * generated Kconfig with a clear separator comment. If no user Kconfig is provided, the generated
   * content will be returned as-is.
   */
  private fun mergeKconfigWithWorkspace(generated: String): String {
    val workspaceKconfig = workspaceRoot.resolve("Kconfig")
    if (!Files.exists(workspaceKconfig)) {
      return generated
    }

    val userConfig = Files.readString(workspaceKconfig)
    if (userConfig.isBlank()) {
      return generated
    }

    return buildString {
      append(generated.trimEnd())
      append("\n\n# ---- User-provided Kconfig overlay ----\n")
      append(userConfig.trimStart())
    }
  }
}

/**
 * Provides Zephyr configuration snippets based on the target board. This allows us to conditionally
 * include board-specific configurations without hardcoding them into the main generator logic.
 *
 * This may be needed for some boards that require specific drivers or settings to even compile the
 * generated code and reactor-uc libraries. We don't want the user to have to know about these
 * details or even be aware that they need to provide their own custom configuration files, so we
 * include them automatically based on the board specified in the target configuration.
 *
 * Currently, this is only used for Raspberry Pi Pico boards, which require additional entropy
 * generator settings.
 */
private class ZephyrBoardConfigProvider {

  private data class BoardConfig(
      val boards: Set<String>,
      val config: String,
  ) {
    fun matches(boardName: String?) = boardName != null && boardName in boards
  }

  /**
   * List of known board configurations. Each entry specifies a set of board names and the
   * corresponding Zephyr configuration snippet to include if the target board matches any of those
   * names.
   */
  private val boardConfigs =
      listOf(
          BoardConfig(
              boards =
                  setOf(
                      "rpi_pico",
                      "rpi_pico2",
                      "rpi_pico_w",
                      "rpi_pico2_w",
                      "raspberrypi_pico",
                      "w5500_evb_pico"),
              config =
                  """
        |# Pico specific configuration
        |
        |CONFIG_SERIAL=y
        |CONFIG_UART_CONSOLE=y
        |CONFIG_STDOUT_CONSOLE=y
        |CONFIG_ENTROPY_GENERATOR=y
        |CONFIG_TEST_RANDOM_GENERATOR=y
        """
                      .trimMargin()),
      )

  /** Returns the Zephyr configuration snippet for the given board name, or null if there are no */
  fun configFor(boardName: String?): String? =
      boardConfigs
          .asSequence()
          .filter { it.matches(boardName) }
          .map { it.config }
          .joinToString(separator = "\n")
          .takeIf { it.isNotBlank() }
}
