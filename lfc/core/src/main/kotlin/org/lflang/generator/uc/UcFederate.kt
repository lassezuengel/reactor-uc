package org.lflang.generator.uc

import org.lflang.AttributeUtils
import org.lflang.TimeValue
import org.lflang.generator.uc.UcInstanceGenerator.Companion.codeTypeFederate
import org.lflang.isBank
import org.lflang.lf.Attribute
import org.lflang.lf.Instantiation
import org.lflang.target.property.type.PlatformType

class UcFederate(val inst: Instantiation, val bankIdx: Int) {
  val isBank = inst.isBank
  val platform: PlatformType.Platform = AttributeUtils.getFederatePlatform(inst)
  val board: String? = AttributeUtils.getFederateBoard(inst)
  val interfaces = mutableListOf<UcNetworkInterface>()
  val codeType = if (isBank) "${inst.codeTypeFederate}_${bankIdx}" else inst.codeTypeFederate
  val name = if (isBank) "${inst.name}_${bankIdx}" else inst.name

  val clockSyncParams: UcClockSyncParameters =
      if (AttributeUtils.getClockSyncAttr(inst) != null)
          UcClockSyncParameters(AttributeUtils.getClockSyncAttr(inst))
      else UcClockSyncParameters()

  constructor(other: UcFederate) : this(other.inst, other.bankIdx)

  fun addInterface(iface: UcNetworkInterface) {
    interfaces.add(iface)
  }

  fun setGrandmaster() {
    clockSyncParams.grandmaster = true
  }

  fun getInterface(name: String): UcNetworkInterface = interfaces.find { it.name == name }!!

  fun getInterfaceByType(type: NetworkChannelType): UcNetworkInterface? =
      interfaces.firstOrNull { it.type == type }

  fun ensureIpInterfaceByType(type: NetworkChannelType): UcNetworkInterface {
    getInterfaceByType(type)?.let {
      return it
    }

    val ip =
        when (val existing =
            interfaces.firstOrNull { it is UcTcpIpInterface || it is UcRudpIpInterface }) {
          is UcTcpIpInterface -> existing.getIpAddress()
          is UcRudpIpInterface -> existing.getIpAddress()
          else ->
              throw IllegalArgumentException(
                  "Clock sync ${type.name.lowercase()} channel requires an IP-based interface on the federate.")
        }

    val created: UcNetworkInterface =
        when (type) {
          NetworkChannelType.TCP_IP -> UcTcpIpInterface(ip, "clock_sync_tcp")
          NetworkChannelType.RUDP_IP -> UcRudpIpInterface(ip, "clock_sync_rudp")
          else ->
              throw IllegalArgumentException(
                  "Clock sync ${type.name.lowercase()} channel is not supported for this interface type.")
        }
    addInterface(created)
    return created
  }

  fun getJoiningPolicy(): JoiningPolicy {
    val attr: Attribute? = AttributeUtils.getJoiningPolicy(inst)
    return attr
        ?.let { JoiningPolicy.parse(it.getAttrParms().get(0).getValue()) }
        .run { JoiningPolicy.JOIN_IMMEDIATELY }
  }

  fun getDefaultInterface(): UcNetworkInterface = interfaces.first()

  fun getCompileDefs(): List<String> =
      interfaces.distinctBy { it.type }.map { it.compileDefs } + "FEDERATED"

  fun getMaxWait(): TimeValue = AttributeUtils.getMaxWait(inst)

  override fun equals(other: Any?): Boolean {
    if (this === other) return true
    if (other !is UcFederate) return false

    val sameInst = inst == other.inst
    val sameBank = bankIdx == other.bankIdx
    return if (isBank) sameInst && sameBank else sameInst
  }
}
