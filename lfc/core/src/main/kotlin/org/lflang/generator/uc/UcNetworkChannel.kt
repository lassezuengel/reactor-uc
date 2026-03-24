package org.lflang.generator.uc

import org.lflang.AttributeUtils.getInterfaceAttributes
import org.lflang.AttributeUtils.getLinkAttribute
import org.lflang.generator.uc.NetworkChannelType.*
import org.lflang.lf.Attribute

// An enumeration of the supported NetworkChannels
enum class NetworkChannelType {
  TCP_IP,
  RUDP_IP,
  CUSTOM,
  COAP_UDP_IP,
  S4NOC,
  UART,
  NONE
}

object UcNetworkInterfaceFactory {
  fun createInterfaces(
      federate: UcFederate,
      allocator: NetworkInterfaceAllocator
  ): List<UcNetworkInterface> {
    val attrs: List<Attribute> = getInterfaceAttributes(federate.inst)
    return if (attrs.isEmpty()) {
      listOf(createDefaultInterface(federate, allocator))
    } else {
      attrs.map { createInterfaceFromAttribute(federate, it, allocator) }
    }
  }

  private fun createInterfaceFromAttribute(
      federate: UcFederate,
      attr: Attribute,
      allocator: NetworkInterfaceAllocator
  ): UcNetworkInterface {
    val protocol = attr.attrName.substringAfter("_")
    return when (protocol) {
      "tcp" -> UcTcpIpInterface.fromAttribute(federate, attr, allocator)
      "rudp" -> UcRudpIpInterface.fromAttribute(federate, attr, allocator)
      "uart" -> UcUARTInterface.fromAttribute(federate, attr)
      "coap" -> UcCoapUdpIpInterface.fromAttribute(federate, attr, allocator)
      "s4noc" -> UcS4NocInterface.fromAttribute(federate, attr)
      "custom" -> UcCustomInterface.fromAttribute(federate, attr)
      else -> throw IllegalArgumentException("Unrecognized interface attribute $attr")
    }
  }

  private fun createDefaultInterface(
      federate: UcFederate,
      allocator: NetworkInterfaceAllocator
  ): UcNetworkInterface {
    val ip = allocator.allocateAddress(federate, null) { IPAddress.fromString("127.0.0.1") }
    return UcTcpIpInterface(ipAddress = ip)
  }
}

// A NetworkEndpoint is a communication endpoint located at the UcNetworkInterface of a federate.
// A NetworkChannel is between two NetworkEndpoints.
abstract class UcNetworkEndpoint(val iface: UcNetworkInterface)

class UcTcpIpEndpoint(val ipAddress: IPAddress, val port: Int, iface: UcTcpIpInterface) :
    UcNetworkEndpoint(iface) {}

class UcRudpIpEndpoint(val ipAddress: IPAddress, val port: Int, iface: UcRudpIpInterface) :
    UcNetworkEndpoint(iface) {}

class UcUARTEndpoint(
    val uart_device: Int,
    val baud_rate: Int,
    val data_bits: UARTDataBits,
    val parity: UARTParityBits,
    val stop_bits: UARTStopBits,
    val async: Boolean,
    iface: UcUARTInterface
) : UcNetworkEndpoint(iface) {}

class UcCoapUdpIpEndpoint(val ipAddress: IPAddress, iface: UcCoapUdpIpInterface) :
    UcNetworkEndpoint(iface) {}

class UcS4NocEndpoint(val core: Int, iface: UcS4NocInterface) : UcNetworkEndpoint(iface) {}

class UcCustomEndpoint(iface: UcCustomInterface) : UcNetworkEndpoint(iface) {}

// A federate can have several NetworkInterfaces, which are specified using attributes in the LF
// program.
// A NetworkInterface has a name and can contain a set of endpoints.
abstract class UcNetworkInterface(val type: NetworkChannelType, val name: String) {
  val endpoints = mutableListOf<UcNetworkEndpoint>()

  /**
   * A header file that should be included to support this NetworkInterface. Used by CustomInterface
   */
  abstract val includeHeaders: String

  /** A compile definition which must be defined to get support for this NetworkInterface */
  abstract val compileDefs: String
}

class UcTcpIpInterface(private val ipAddress: IPAddress, name: String? = null) :
    UcNetworkInterface(TCP_IP, name ?: "tcp") {
  private val portManager = IpAddressManager.getPortManager(ipAddress)
  override val includeHeaders: String = ""
  override val compileDefs: String = "NETWORK_CHANNEL_TCP_POSIX"

  fun getIpAddress(): IPAddress = ipAddress

  fun createEndpoint(port: Int?): UcTcpIpEndpoint {
    val portNum =
        if (port != null) {
          portManager.reservePortNumber(port)
          port
        } else {
          portManager.acquirePortNumber()
        }
    val ep = UcTcpIpEndpoint(ipAddress, portNum, this)
    endpoints.add(ep)
    return ep
  }

  companion object {
    fun fromAttribute(
        federate: UcFederate,
        attr: Attribute,
        allocator: NetworkInterfaceAllocator
    ): UcTcpIpInterface {
      val name = attr.getParamString("name")
      val ip = allocator.allocateAddress(federate, attr) { IPAddress.fromString("127.0.0.1") }
      return UcTcpIpInterface(ip, name)
    }
  }
}

class UcRudpIpInterface(private val ipAddress: IPAddress, name: String? = null) :
    UcNetworkInterface(RUDP_IP, name ?: "rudp") {
  private val portManager = IpAddressManager.getPortManager(ipAddress)
  override val includeHeaders: String = ""
  override val compileDefs: String = "NETWORK_CHANNEL_RUDP_IP"

  fun getIpAddress(): IPAddress = ipAddress

  fun createEndpoint(port: Int?): UcRudpIpEndpoint {
    val portNum =
        if (port != null) {
          portManager.reservePortNumber(port)
          port
        } else {
          portManager.acquirePortNumber()
        }
    val ep = UcRudpIpEndpoint(ipAddress, portNum, this)
    endpoints.add(ep)
    return ep
  }

  companion object {
    fun fromAttribute(
        federate: UcFederate,
        attr: Attribute,
        allocator: NetworkInterfaceAllocator
    ): UcRudpIpInterface {
      val name = attr.getParamString("name")
      val ip = allocator.allocateAddress(federate, attr) { IPAddress.fromString("127.0.0.1") }
      return UcRudpIpInterface(ip, name)
    }
  }
}

class UcUARTInterface(
    private val uartDevice: Int,
    private val baudRate: Int,
    private val dataBits: UARTDataBits,
    private val parity: UARTParityBits,
    private val stopBits: UARTStopBits,
    private val async: Boolean,
    name: String? = null
) : UcNetworkInterface(UART, name ?: "uart") {

  override val includeHeaders: String = ""
  override val compileDefs: String = "NETWORK_CHANNEL_UART"

  fun createEndpoint(): UcUARTEndpoint {
    val ep = UcUARTEndpoint(uartDevice, baudRate, dataBits, parity, stopBits, async, this)
    endpoints.add(ep)
    return ep
  }

  companion object {
    fun fromAttribute(federate: UcFederate, attr: Attribute): UcUARTInterface {
      val uartDevice = attr.getParamInt("uart_device") ?: 0
      val baudRate = attr.getParamInt("baud_rate") ?: 9600
      val dataBits = UARTDataBitsFromInteger(attr.getParamInt("data_bits") ?: 8)
      val parity = UARTParityBits.valueOf(attr.getParamString("parity").toString())
      val uartStopBits = UARTStopBitsFromInteger(attr.getParamInt("stop_bits") ?: 1)
      val async = attr.getParamString("async").toBoolean()
      val name = attr.getParamString("name")
      UARTDeviceManager.reserve(uartDevice)
      return UcUARTInterface(uartDevice, baudRate, dataBits, parity, uartStopBits, async, name)
    }
  }
}

class UcCoapUdpIpInterface(private val ipAddress: IPAddress, name: String? = null) :
    UcNetworkInterface(COAP_UDP_IP, name ?: "coap") {
  override val includeHeaders: String = ""
  override val compileDefs: String = "NETWORK_CHANNEL_COAP_UDP"

  fun createEndpoint(): UcCoapUdpIpEndpoint {
    val ep = UcCoapUdpIpEndpoint(ipAddress, this)
    endpoints.add(ep)
    return ep
  }

  companion object {
    fun fromAttribute(
        federate: UcFederate,
        attr: Attribute,
        allocator: NetworkInterfaceAllocator
    ): UcCoapUdpIpInterface {
      val name = attr.getParamString("name")
      val ip = allocator.allocateAddress(federate, attr) { IPAddress.fromString("127.0.0.1") }
      return UcCoapUdpIpInterface(ip, name)
    }
  }
}

class UcS4NocInterface(val core: Int, name: String? = null) :
    UcNetworkInterface(S4NOC, name ?: "s4noc") {
  override val includeHeaders: String = ""
  override val compileDefs: String = "NETWORK_CHANNEL_S4NOC"

  init {
    println("UcS4NocInterface created with core=$core and name=${name ?: "s4noc"}")
  }

  fun createEndpoint(): UcS4NocEndpoint {
    val ep = UcS4NocEndpoint(core, this)
    endpoints.add(ep)
    return ep
  }

  companion object {
    fun fromAttribute(federate: UcFederate, attr: Attribute): UcS4NocInterface {
      val core = attr.getParamInt("core") ?: 0
      val name = attr.getParamString("name")
      return UcS4NocInterface(core, name)
    }
  }
}

class UcCustomInterface(name: String, val include: String, val args: String? = null) :
    UcNetworkInterface(CUSTOM, name) {
  override val compileDefs = ""
  override val includeHeaders: String = "#include \"$include\""

  fun createEndpoint(): UcCustomEndpoint {
    val ep = UcCustomEndpoint(this)
    endpoints.add(ep)
    return ep
  }

  companion object {
    fun fromAttribute(federate: UcFederate, attr: Attribute): UcCustomInterface {
      val name = attr.getParamString("name")
      val include = attr.getParamString("include")
      val args = attr.getParamString("args")
      return UcCustomInterface(name!!, include!!, args)
    }
  }
}

/** A UcNetworkChannel is created by giving two endpoints and deciding which one is the server */
abstract class UcNetworkChannel(
    val type: NetworkChannelType,
    val src: UcNetworkEndpoint,
    val dest: UcNetworkEndpoint,
    val serverLhs: Boolean,
) {
  /** Generate code calling the constructor of the source endpoint */
  abstract fun generateChannelCtorSrc(): String

  /** Generate code calling the constructor of the destination endpoint */
  abstract fun generateChannelCtorDest(): String

  open fun supportsClockSyncUdpChannel(): Boolean = false

  open fun generateClockSyncUdpChannelCtorSrc(
      srcClockSyncInterface: UcNetworkInterface? = null,
      destClockSyncInterface: UcNetworkInterface? = null
  ): String? = null

  open fun generateClockSyncUdpChannelCtorDest(
      srcClockSyncInterface: UcNetworkInterface? = null,
      destClockSyncInterface: UcNetworkInterface? = null
  ): String? = null

  abstract val codeType: String

  companion object {
    /**
     * Given a FederatedConnection bundle which contains an LF connection and all the connection
     * channels. Create an endpoint at source and destination and a UcNetworkChannel connecting the,
     */
    fun createNetworkEndpointsAndChannelForBundle(
        bundle: UcFederatedConnectionBundle,
        useIpv6: Boolean
    ): UcNetworkChannel {
      val attr: Attribute? = getLinkAttribute(bundle.groupedConnections.first().lfConn)
      var srcIf: UcNetworkInterface
      var destIf: UcNetworkInterface
      var channel: UcNetworkChannel
      var serverLhs = true
      var serverPort: Int? = null

      if (attr == null) {
        // If there is no @link attribute on the connection we just get the default (unless there
        //  is ambiguity)
        srcIf = bundle.src.getDefaultInterface()
        destIf = bundle.dest.getDefaultInterface()
      } else {
        // Parse the @link attribute and generate a UcNetworkChannel between the correct
        // interfaces.
        val srcIfName = attr.getParamString("left")
        val destIfName = attr.getParamString("right")
        val serverSideAttr = attr.getParamString("server_side")
        serverPort = attr.getParamInt("server_port")
        srcIf =
            if (srcIfName != null) bundle.src.getInterface(srcIfName)
            else bundle.src.getDefaultInterface()
        destIf =
            if (destIfName != null) bundle.dest.getInterface(destIfName)
            else bundle.dest.getDefaultInterface()
        serverLhs = if (serverSideAttr == null) true else !serverSideAttr!!.equals("right")
      }

      require(srcIf.type == destIf.type)
      when (srcIf.type) {
        TCP_IP -> {
          val srcEp =
              (srcIf as UcTcpIpInterface).createEndpoint(if (serverLhs) serverPort else null)
          val destEp =
              (destIf as UcTcpIpInterface).createEndpoint(if (!serverLhs) serverPort else null)
          channel = UcTcpIpChannel(srcEp, destEp, serverLhs, useIpv6)
        }

        RUDP_IP -> {
          val srcEp =
              (srcIf as UcRudpIpInterface).createEndpoint(if (serverLhs) serverPort else null)
          val destEp =
              (destIf as UcRudpIpInterface).createEndpoint(if (!serverLhs) serverPort else null)
          channel = UcRudpIpChannel(srcEp, destEp, serverLhs, useIpv6)
        }

        UART -> {
          val srcEp = (srcIf as UcUARTInterface).createEndpoint()
          val destEp = (destIf as UcUARTInterface).createEndpoint()
          channel = UcUARTChannel(srcEp, destEp)
        }

        COAP_UDP_IP -> {
          val srcEp = (srcIf as UcCoapUdpIpInterface).createEndpoint()
          val destEp = (destIf as UcCoapUdpIpInterface).createEndpoint()
          channel = UcCoapUdpIpChannel(srcEp, destEp)
        }
        S4NOC -> {
          val srcEp = (srcIf as UcS4NocInterface).createEndpoint()
          val destEp = (destIf as UcS4NocInterface).createEndpoint()
          channel = UcS4NocChannel(srcEp, destEp)
        }
        CUSTOM -> {
          val srcEp = (srcIf as UcCustomInterface).createEndpoint()
          val destEp = (destIf as UcCustomInterface).createEndpoint()
          channel = UcCustomChannel(srcEp, destEp)
        }

        NONE -> throw IllegalArgumentException("Tried creating network channel with type=NONE")
      }
      return channel
    }
  }
}

private data class ClockSyncIpEndpoint(val ipAddress: IPAddress, val port: Int)

private fun clockSyncEndpointFromInterface(iface: UcNetworkInterface): ClockSyncIpEndpoint {
  return when (iface) {
    is UcTcpIpInterface -> {
      val ep = iface.createEndpoint(null)
      ClockSyncIpEndpoint(ep.ipAddress, ep.port)
    }
    is UcRudpIpInterface -> {
      val ep = iface.createEndpoint(null)
      ClockSyncIpEndpoint(ep.ipAddress, ep.port)
    }
    else ->
        throw IllegalArgumentException(
            "Clock sync interface must be TCP or RUDP; got ${iface.type} (${iface.name}).")
  }
}

private fun allocateClockSyncEndpointForMainChannel(
    endpoint: UcNetworkEndpoint
): ClockSyncIpEndpoint {
  return clockSyncEndpointFromInterface(endpoint.iface)
}

private fun protocolFamilyForIp(ip: IPAddress): String {
  return when (ip) {
    is IPAddress.IPv4 -> "AF_INET"
    is IPAddress.IPv6 -> "AF_INET6"
    else -> throw IllegalArgumentException("Unknown IP address type")
  }
}

private fun buildClockSyncCtor(
    localEndpoint: ClockSyncIpEndpoint,
    remoteEndpoint: ClockSyncIpEndpoint
): String {
  val family = protocolFamilyForIp(localEndpoint.ipAddress)
  return "UdpIpChannel_ctor(&self->clock_sync_channel, \"${localEndpoint.ipAddress.address}\", ${localEndpoint.port}, \"${remoteEndpoint.ipAddress.address}\", ${remoteEndpoint.port}, ${family});"
}

private fun resolveClockSyncEndpoints(
    srcMainEndpoint: UcNetworkEndpoint,
    destMainEndpoint: UcNetworkEndpoint,
    srcClockSyncInterface: UcNetworkInterface?,
    destClockSyncInterface: UcNetworkInterface?
): Pair<ClockSyncIpEndpoint, ClockSyncIpEndpoint> {
  val srcEndpoint =
      srcClockSyncInterface?.let { clockSyncEndpointFromInterface(it) }
          ?: allocateClockSyncEndpointForMainChannel(srcMainEndpoint)
  val destEndpoint =
      destClockSyncInterface?.let { clockSyncEndpointFromInterface(it) }
          ?: allocateClockSyncEndpointForMainChannel(destMainEndpoint)
  return Pair(srcEndpoint, destEndpoint)
}

class UcTcpIpChannel(
    src: UcTcpIpEndpoint,
    dest: UcTcpIpEndpoint,
    serverLhs: Boolean = true,
    private val useIpv6: Boolean = false,
) : UcNetworkChannel(TCP_IP, src, dest, serverLhs) {
  private val srcTcp = src
  private val destTcp = dest
  private val protocolFamily = if (useIpv6) "AF_INET6" else "AF_INET"
  private val serverEndpoint = if (serverLhs) srcTcp else destTcp
  private val serverAddress = serverEndpoint.ipAddress.address
  private val serverPort = serverEndpoint.port
  private var clockSyncEndpoints: Pair<ClockSyncIpEndpoint, ClockSyncIpEndpoint>? = null

  private fun getClockSyncEndpoints(
      srcClockSyncInterface: UcNetworkInterface?,
      destClockSyncInterface: UcNetworkInterface?
  ): Pair<ClockSyncIpEndpoint, ClockSyncIpEndpoint> {
    if (clockSyncEndpoints == null) {
      clockSyncEndpoints =
          resolveClockSyncEndpoints(
              srcTcp, destTcp, srcClockSyncInterface, destClockSyncInterface)
    }
    return clockSyncEndpoints!!
  }

  private fun ctorString(isServer: Boolean): String {
    val role = if (isServer) "true" else "false"
    return "TcpIpChannel_ctor(&self->channel, \"${serverAddress}\", ${serverPort}, ${protocolFamily}, ${role});"
  }

  override fun generateChannelCtorSrc(): String {
    return ctorString(serverLhs)
  }

  override fun generateChannelCtorDest(): String {
    return ctorString(!serverLhs)
  }

  fun generateChannelCtorForRole(isServer: Boolean): String = ctorString(isServer)

  override fun supportsClockSyncUdpChannel(): Boolean = true

  override fun generateClockSyncUdpChannelCtorSrc(
      srcClockSyncInterface: UcNetworkInterface?,
      destClockSyncInterface: UcNetworkInterface?
  ): String {
    val (srcEndpoint, destEndpoint) =
        getClockSyncEndpoints(srcClockSyncInterface, destClockSyncInterface)
    return buildClockSyncCtor(srcEndpoint, destEndpoint)
  }

  override fun generateClockSyncUdpChannelCtorDest(
      srcClockSyncInterface: UcNetworkInterface?,
      destClockSyncInterface: UcNetworkInterface?
  ): String {
    val (srcEndpoint, destEndpoint) =
        getClockSyncEndpoints(srcClockSyncInterface, destClockSyncInterface)
    return buildClockSyncCtor(destEndpoint, srcEndpoint)
  }

  override val codeType: String
    get() = "TcpIpChannel"
}

class UcRudpIpChannel(
    src: UcRudpIpEndpoint,
    dest: UcRudpIpEndpoint,
    serverLhs: Boolean = true,
    private val useIpv6: Boolean = false,
) : UcNetworkChannel(RUDP_IP, src, dest, serverLhs) {
  private val srcRudp = src
  private val destRudp = dest
  private val protocolFamily = if (useIpv6) "AF_INET6" else "AF_INET"
  private var clockSyncEndpoints: Pair<ClockSyncIpEndpoint, ClockSyncIpEndpoint>? = null

  private fun getClockSyncEndpoints(
      srcClockSyncInterface: UcNetworkInterface?,
      destClockSyncInterface: UcNetworkInterface?
  ): Pair<ClockSyncIpEndpoint, ClockSyncIpEndpoint> {
    if (clockSyncEndpoints == null) {
      clockSyncEndpoints =
          resolveClockSyncEndpoints(
              srcRudp, destRudp, srcClockSyncInterface, destClockSyncInterface)
    }
    return clockSyncEndpoints!!
  }

  private fun ctorString(local: UcRudpIpEndpoint, remote: UcRudpIpEndpoint): String {
    return "RUdpIpChannel_ctor(&self->channel, \"${local.ipAddress.address}\", ${local.port}, \"${remote.ipAddress.address}\", ${remote.port}, ${protocolFamily});"
  }

  override fun generateChannelCtorSrc(): String {
    return ctorString(srcRudp, destRudp)
  }

  override fun generateChannelCtorDest(): String {
    return ctorString(destRudp, srcRudp)
  }

  override fun supportsClockSyncUdpChannel(): Boolean = true

  override fun generateClockSyncUdpChannelCtorSrc(
      srcClockSyncInterface: UcNetworkInterface?,
      destClockSyncInterface: UcNetworkInterface?
  ): String {
    val (srcEndpoint, destEndpoint) =
        getClockSyncEndpoints(srcClockSyncInterface, destClockSyncInterface)
    return buildClockSyncCtor(srcEndpoint, destEndpoint)
  }

  override fun generateClockSyncUdpChannelCtorDest(
      srcClockSyncInterface: UcNetworkInterface?,
      destClockSyncInterface: UcNetworkInterface?
  ): String {
    val (srcEndpoint, destEndpoint) =
        getClockSyncEndpoints(srcClockSyncInterface, destClockSyncInterface)
    return buildClockSyncCtor(destEndpoint, srcEndpoint)
  }

  override val codeType: String
    get() = "RUdpIpChannel"
}

class UcUARTChannel(private val uart_src: UcUARTEndpoint, private val uart_dest: UcUARTEndpoint) :
    UcNetworkChannel(UART, uart_src, uart_dest, false) {

  override fun generateChannelCtorSrc() =
      "Uart${if (uart_src.async) "Async" else "Polled"}Channel_ctor(&self->channel, ${uart_src.uart_device}, ${uart_src.baud_rate}, UC_${uart_src.data_bits}, UC_${uart_src.parity}, UC_${uart_src.stop_bits});"

  override fun generateChannelCtorDest() =
      "Uart${if (uart_dest.async) "Async" else "Polled"}Channel_ctor(&self->channel, ${uart_dest.uart_device}, ${uart_dest.baud_rate}, UC_${uart_dest.data_bits}, UC_${uart_dest.parity}, UC_${uart_dest.stop_bits});"

  override val codeType: String
    get() =
        "Uart${if (uart_src.async) "Async" else "Polled"}Channel" // TODO: this is a problem if the
  // different sides use different
  // implementations FIXME
}

class UcCoapUdpIpChannel(
    src: UcCoapUdpIpEndpoint,
    dest: UcCoapUdpIpEndpoint,
    // TODO: In CoAP every node is a server and a client => default server to false for now
) : UcNetworkChannel(COAP_UDP_IP, src, dest, false) {
  private val srcAddr = src
  private val destAddr = dest

  private fun getIpProtocolFamily(ip: IPAddress): String {
    return when (ip) {
      is IPAddress.IPv4 -> "AF_INET"
      is IPAddress.IPv6 -> "AF_INET6"
      else -> throw IllegalArgumentException("Unknown IP address type")
    }
  }

  override fun generateChannelCtorSrc() =
      "CoapUdpIpChannel_ctor(&self->channel, \"${destAddr.ipAddress.address}\", ${getIpProtocolFamily(destAddr.ipAddress)});"

  override fun generateChannelCtorDest() =
      "CoapUdpIpChannel_ctor(&self->channel, \"${srcAddr.ipAddress.address}\", ${getIpProtocolFamily(srcAddr.ipAddress)});"

  override val codeType: String
    get() = "CoapUdpIpChannel"
}

class UcS4NocChannel(
    src: UcS4NocEndpoint,
    dest: UcS4NocEndpoint,
) : UcNetworkChannel(S4NOC, src, dest, false) {
  private val srcS4Noc = src
  private val destS4Noc = dest

  override fun generateChannelCtorSrc() = "S4NOCPollChannel_ctor(&self->channel, ${srcS4Noc.core});"

  override fun generateChannelCtorDest() =
      "S4NOCPollChannel_ctor(&self->channel, ${destS4Noc.core});"

  override val codeType: String
    get() = "S4NOCPollChannel"
}

class UcCustomChannel(
    src: UcCustomEndpoint,
    dest: UcCustomEndpoint,
    serverLhs: Boolean = true,
) : UcNetworkChannel(CUSTOM, src, dest, serverLhs) {
  val srcIface = src.iface as UcCustomInterface
  val destIface = dest.iface as UcCustomInterface
  private val srcArgs = if (srcIface.args != null) ", ${srcIface.args}" else ""
  private val destArgs = if (destIface.args != null) ", ${destIface.args}" else ""

  override fun generateChannelCtorSrc() = "${srcIface.name}_ctor(&self->channel, ${srcArgs});"

  override fun generateChannelCtorDest() = "${destIface.name}_ctor(&self->channel, ${destArgs});"

  override val codeType: String
    get() = srcIface.name
}
