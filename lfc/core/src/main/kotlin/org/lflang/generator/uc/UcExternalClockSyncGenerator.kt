package org.lflang.generator.uc

import org.lflang.AttributeUtils
import org.lflang.lf.Reactor

class UcExternalClockSyncGenerator(private val reactor: Reactor) {

  companion object {
    const val numSystemEventsConst = 1
    const val instName = "external_clock_sync"

    fun getNumSystemEvents(): Int = numSystemEventsConst
  }

  private val attr = AttributeUtils.findAttributeByName(reactor, "external_clock_sync")

  fun enabled(): Boolean {
    val status = attr?.getParamString("status")?.lowercase()
    return attr != null && status == "on"
  }

  fun generateSelfStruct() =
      if (enabled()) "LF_DEFINE_EXTERNAL_CLOCK_SYNC_STRUCT(Federate, ${numSystemEventsConst})" else ""

  fun generateCtor() =
      if (enabled()) "LF_DEFINE_EXTERNAL_CLOCK_SYNC_CTOR(Federate, ${numSystemEventsConst});" else ""

  fun generateFederateStructField() =
      if (enabled()) "FederateExternalClockSync ${instName};" else ""

  fun generateFederateCtorCode() = if (enabled()) "LF_INITIALIZE_EXTERNAL_CLOCK_SYNC(Federate);" else ""
}
