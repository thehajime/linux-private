"""Stable report order; internal artifact names remain unchanged."""

PROFILES = (
    ("um-mmu", "um-mmu(p)", "#27ae60"),
    ("um-mmu-seccomp", "um-mmu(s)", "#145a32"),
    ("um-mmu-tt-seccomp", "um-mmu-tt(s)", "#196f3d"),
    ("um-nommu", "um-nommu(p)", "#d68910"),
    ("um-nommu-seccomp", "um-nommu(s)", "#a04000"),
    ("um-nommu-tt-seccomp", "um-nommu-tt(s)", "#873600"),
    ("um-nommu-plain-sas", "um-nommu-sas(s)", "#566573"),
    ("um-nommu-plain-sas-zpoline", "um-nommu-sas(z)", "#85929e"),
    ("um-nommu-sas-seccomp", "um-nommu-sas-swmmu(s)", "#0000ff"),
    ("um-nommu-sas-zpoline", "um-nommu-sas-swmmu(z)", "#4169e1"),
    ("native", "(native)", "#c0392b"),
)
PROFILE_LABELS = {target: label for target, label, _color in PROFILES}
PROFILE_RANK = {target: rank for rank, (target, _label, _color) in enumerate(PROFILES)}
# KUnit validates a kernel build, shared by its syscall interception modes.
BUILD_LABELS = {
    "um-mmu": "um-mmu(p/s)",
    "um-mmu-tt": "um-mmu-tt(s)",
    "um-nommu": "um-nommu(p/s)",
    "um-nommu-tt": "um-nommu-tt(s)",
    "um-nommu-plain-sas": "um-nommu-sas(s/z)",
    "um-nommu-sas": "um-nommu-sas-swmmu(s/z)",
}
BUILD_TARGETS = {
    "um-mmu-tt": "um-mmu-tt-seccomp",
    "um-nommu-tt": "um-nommu-tt-seccomp",
    "um-nommu-sas": "um-nommu-sas-seccomp",
}


def profile_sort_key(name):
    target, _separator, suite = name.partition("/")
    target = BUILD_TARGETS.get(target, target)
    return PROFILE_RANK.get(target, len(PROFILES)), target, suite


def display_profile(name, *, build=False):
    target, separator, suite = name.partition("/")
    labels = BUILD_LABELS if build else PROFILE_LABELS
    return labels.get(target, target) + separator + suite
