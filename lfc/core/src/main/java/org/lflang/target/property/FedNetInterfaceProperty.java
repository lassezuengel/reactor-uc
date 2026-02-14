package org.lflang.target.property;

import org.lflang.MessageReporter;
import org.lflang.ast.ASTUtils;
import org.lflang.lf.Element;
import org.lflang.lf.LfPackage.Literals;
import org.lflang.target.TargetConfig;
import org.lflang.target.property.type.FedNetInterfaceType;
import org.lflang.target.property.type.FedNetInterfaceType.FedNetInterface;
import org.lflang.target.property.type.PlatformType.Platform;

/** Directive to choose the network interface used for federated execution. */
public final class FedNetInterfaceProperty
    extends TargetProperty<FedNetInterface, FedNetInterfaceType> {

  /** Singleton target property instance. */
  public static final FedNetInterfaceProperty INSTANCE = new FedNetInterfaceProperty();

  private FedNetInterfaceProperty() {
    super(new FedNetInterfaceType());
  }

  @Override
  public FedNetInterface initialValue() {
    return FedNetInterface.getDefault();
  }

  @Override
  protected FedNetInterface fromString(String string, MessageReporter reporter) {
    return this.type.forName(string);
  }

  @Override
  public FedNetInterface fromAst(Element node, MessageReporter reporter) {
    return fromString(ASTUtils.elementToSingleString(node), reporter);
  }

  @Override
  public Element toAstElement(FedNetInterface value) {
    return ASTUtils.toElement(value.toString());
  }

  @Override
  public String name() {
    return "net-interface";
  }

  @Override
  public void validate(TargetConfig config, MessageReporter reporter) {
    var pair = config.lookup(this);
    if (pair == null) {
      return;
    }

    if (!config.isFederated()) {
      reporter
          .at(pair, Literals.KEY_VALUE_PAIR__NAME)
          .error("The fednet-interface target property requires a federated program.");
      return;
    }

    if (config.get(this) == FedNetInterface.SICSLOWPAN) {
      var platform = config.get(PlatformProperty.INSTANCE).platform();
      if (platform != Platform.ZEPHYR) {
        reporter
            .at(pair, Literals.KEY_VALUE_PAIR__VALUE)
            .warning( // TODO: should be error
                "The sicslowpan network interface requires the platform target property to be set"
                    + " to \"zephyr\".");
      }
    }
  }
}