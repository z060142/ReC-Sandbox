"""List the contents of an OCIO config: name, version, colour spaces, displays, views.

Usage:
    uv run list_config.py [config-uri ...]
    uv run list_config.py --builtins        # list every registered built-in config
"""

from __future__ import annotations

import argparse
import sys

import PyOpenColorIO as ocio

DEFAULT_CONFIG = "ocio://studio-config-latest"


def list_builtins() -> None:
    reg = ocio.BuiltinConfigRegistry()
    print("Registered built-in configs (OCIO %s):" % ocio.GetVersion())
    for name in reg.getBuiltinConfigs():
        # (name, ui_name, is_recommended, is_default) in OCIO >= 2.3
        if isinstance(name, (tuple, list)):
            n, ui, recommended, default = (list(name) + [None] * 4)[:4]
            flags = []
            if recommended:
                flags.append("recommended")
            if default:
                flags.append("default")
            print("  ocio://%s" % n)
            print("      %s%s" % (ui, ("  [" + ", ".join(flags) + "]") if flags else ""))
        else:
            print("  ocio://%s" % name)


def load(uri: str) -> ocio.Config:
    if uri.startswith("ocio://"):
        return ocio.Config.CreateFromBuiltinConfig(uri[len("ocio://") :])
    return ocio.Config.CreateFromFile(uri)


def dump(uri: str) -> None:
    print("=" * 78)
    print("config URI : %s" % uri)
    cfg = load(uri)
    print("name       : %s" % cfg.getName())
    print("version    : %d.%d" % (cfg.getMajorVersion(), cfg.getMinorVersion()))
    desc = (cfg.getDescription() or "").strip()
    if desc:
        print("description:")
        for line in desc.splitlines():
            print("    %s" % line)
    print()

    print("-- roles --")
    for role, space in cfg.getRoles():
        print("  %-24s -> %s" % (role, space))
    print()

    print("-- colour spaces --")
    for cs in cfg.getColorSpaces():
        print("  %-46s [%s]" % (cs.getName(), cs.getFamily()))
    print()

    print("-- displays and views --")
    print("  active displays: %s" % cfg.getActiveDisplays())
    for display in cfg.getDisplays():
        print("  display: %s" % display)
        for view in cfg.getViews(display):
            cs = cfg.getDisplayViewColorSpaceName(display, view)
            vt = cfg.getDisplayViewTransformName(display, view)
            extra = []
            if vt:
                extra.append("view_transform=%s" % vt)
            if cs:
                extra.append("colorspace=%s" % cs)
            print("      view: %-42s %s" % (view, "  ".join(extra)))
    print()

    print("-- view transforms --")
    for vt in cfg.getViewTransforms():
        print("  %s" % vt.getName())
    print()

    print("-- named transforms --")
    for nt in cfg.getNamedTransforms():
        print("  %s" % nt.getName())
    print()


def main(argv: list[str]) -> int:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("uris", nargs="*", default=None, help="config URIs or file paths")
    ap.add_argument("--builtins", action="store_true", help="list registered built-in configs")
    args = ap.parse_args(argv)

    print("PyOpenColorIO %s" % ocio.__version__)
    if args.builtins:
        list_builtins()
        print()
    for uri in (args.uris or [DEFAULT_CONFIG]):
        dump(uri)
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
