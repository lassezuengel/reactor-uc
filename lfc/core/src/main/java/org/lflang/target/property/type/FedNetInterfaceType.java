package org.lflang.target.property.type;

import org.lflang.target.property.type.FedNetInterfaceType.FedNetInterface;

/** Enumeration of possible network interfaces for federated execution. */
public class FedNetInterfaceType extends OptionsType<FedNetInterface> {

  @Override
  protected Class<FedNetInterface> enumClass() {
    return FedNetInterface.class;
  }

  /** Enumeration of network interfaces. */
  public enum FedNetInterface {
    ETHERNET,
    SICSLOWPAN;

    /** Return the name in lower case. */
    @Override
    public String toString() {
      return this.name().toLowerCase();
    }

    public static FedNetInterface getDefault() {
      return FedNetInterface.ETHERNET;
    }
  }
}
