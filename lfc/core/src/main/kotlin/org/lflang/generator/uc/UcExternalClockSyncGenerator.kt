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
  private val modulePath = attr?.getParamString("module")?.trim()
  private val apiName = "external_clock_sync_api"

  fun enabled(): Boolean {
    return !modulePath.isNullOrBlank()
  }

  fun generateModuleInclude() = if (enabled()) "#include ${modulePath}" else ""

  fun generateApiStruct() =
      if (enabled())
          "static const ExternalClockSyncApi ${apiName} = { .init = lf_clock_sync_init, .schedule = lf_clock_sync_schedule };"
      else ""

  fun generateSelfStruct() =
      if (enabled()) "LF_DEFINE_EXTERNAL_CLOCK_SYNC_STRUCT(Federate, ${numSystemEventsConst})" else ""

    fun generateCtor() =
      if (enabled())
        "LF_DEFINE_EXTERNAL_CLOCK_SYNC_CTOR(Federate, ${numSystemEventsConst}, ${apiName});"
      else ""

  fun generateFederateStructField() =
      if (enabled()) "FederateExternalClockSync ${instName};" else ""

  fun generateFederateCtorCode() = if (enabled()) "LF_INITIALIZE_EXTERNAL_CLOCK_SYNC(Federate);" else ""
}
