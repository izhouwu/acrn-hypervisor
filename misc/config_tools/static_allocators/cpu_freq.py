#!/usr/bin/env python3
#
# Copyright (C) 2022 Intel Corporation.
#
# SPDX-License-Identifier: BSD-3-Clause
#

import common, board_cfg_lib

# CPU frequency dependency
# Some CPU cores may share the same clock domain/group with others, which makes them always run at
# the same frequency of the highest on in the group. Including those conditions:
#   1. CPU in the clock domain described in ACPI _PSD.
#   2. CPU threads sharing the same physical core.
#   3. E-cores residents in the same group.
def alloc_dependency(board_etree, scenario_etree, allocation_etree):
    cpus = board_etree.xpath("//processors//thread")
    for cpu in cpus:
        cpu_id = common.get_node("./cpu_id/text()", cpu)
        psd_cpus = common.get_node("./freqdomain_cpus/text()", cpu).split(' ')
        print("psd_cpus", psd_cpus)
        apic_id = int(common.get_node("./apic_id/text()", cpu)[2:], base=16)
        print("apic", apic_id)
        is_hybrid = (len(board_etree.xpath("//processors//capability[@id='hybrid']")) != 0)
        core_type = common.get_node("./core_type/text()", cpu)
        
        for other_cpu in cpus:
            other_cpu_id = common.get_node("./cpu_id/text()", other_cpu)
            if cpu_id != other_cpu_id:
                other_apic_id = int(common.get_node("./apic_id/text()", other_cpu)[2:], base=16)
                other_core_type = common.get_node("./core_type/text()", other_cpu)
                # threads at same core
                if (apic_id & ~1) == (other_apic_id & ~1):
                    psd_cpus.append(other_cpu_id)
                # e-cores in the same group
                if is_hybrid and core_type == 'Atom' and other_core_type == 'Atom' and (apic_id & ~7) == (other_apic_id & ~7):
                    psd_cpus.append(other_cpu_id)
        print(psd_cpus)
        alloc_dep_node = common.append_node("/acrn-config/hv/cpufreq/CPU", None, allocation_etree, id=cpu_id)
        if psd_cpus != None:
            psd_cpus = list(set(psd_cpus))
            psd_cpus.sort()
            print("append dependency", " ".join(psd_cpus))
            common.append_node("./freq_dependency", " ".join(psd_cpus), alloc_dep_node)
    
# CPU frequency policy:
#
# Frequency policy is used to determine in what range the governor could adjust the CPU's performance.
# eg. highest_performance_lvl, lowest_performance_lvl
# The performance level is not necessary CPU's frequency ratio.
# When using HWP, the levels represents the level used in HWP_REQUEST MSR, while using ACPI p-state, it represents
# the Px level 'x' descripted in ACPI _PSS table.
#
# When the CPU is assigned to a RTVM, we wish to have its frequency fixed, to get more certainty in latency.
# The way to fix the frequency is to set highest_lvl = lowest_lvl.
# 
# Some CPU cores may have dependent frequency, as a frequency domain or group. Their frequency always stays
# the same with the highest one in the group. In this siduation, the CPU's frequency policy should be adjusted
# to align with other CPUs in its group.
def alloc_policy(board_etree, scenario_etree, allocation_etree):
    cpu_has_eist = (len(board_etree.xpath("//processors//capability[@id='est']")) != 0)
    cpu_has_hwp = (len(board_etree.xpath("//processors//capability[@id='hwp_supported']")) != 0)
    cpu_has_turbo = (len(board_etree.xpath("//processors//capability[@id='turbo_boost_available']")) != 0)
    governor = common.get_node("//CPUFREQ_GOVERNOR/text()", scenario_etree)
    freq_interface = common.get_node("//CPUFREQ_INTERFACE/text()", scenario_etree)
    rtvm_cpus = scenario_etree.xpath(f"//vm[vm_type = 'RTVM']//cpu_affinity//pcpu_id/text()")
    cpus = board_etree.xpath("//processors//thread")

    print("111111", freq_interface)
    common.append_node(f"/acrn-config/hv/cpufreq/governor", governor, allocation_etree)

    if cpu_has_hwp and freq_interface == 'CPUFREQ_INTERFACE_HWP':
        common.append_node(f"/acrn-config/hv/cpufreq/interface", "CPUFREQ_INTERFACE_HWP", allocation_etree)
        for cpu in cpus:
            cpu_id = common.get_node("./cpu_id/text()", cpu)
            print(cpu_id)
            guaranteed_performance_lvl = common.get_node("./guaranteed_performance_lvl/text()", cpu)
            highest_performance_lvl = common.get_node("./highest_performance_lvl/text()", cpu)
            lowest_performance_lvl = common.get_node("./lowest_performance_lvl/text()", cpu)
            print(guaranteed_performance_lvl, highest_performance_lvl, lowest_performance_lvl)
            if cpu_id in rtvm_cpus:
                # for CPUs in RTVM, fix to base performance
                policy_lowest = guaranteed_performance_lvl
                policy_highest = guaranteed_performance_lvl
                policy_guaranteed = guaranteed_performance_lvl
            elif cpu_has_turbo:
                policy_lowest = lowest_performance_lvl
                policy_highest = highest_performance_lvl
                policy_guaranteed = guaranteed_performance_lvl
            else:
                policy_lowest = lowest_performance_lvl
                policy_highest = guaranteed_performance_lvl
                policy_guaranteed = guaranteed_performance_lvl
            cpu_node = common.get_node(f"//hv/cpufreq/CPU[@id='{cpu_id}']", allocation_etree)
            policy_node = common.append_node("./policy", None, cpu_node)
            print(cpu_id, policy_guaranteed, policy_highest, policy_lowest)
            common.append_node("./policy_guaranteed_lvl", policy_guaranteed, policy_node)
            common.append_node("./policy_highest_lvl", policy_highest, policy_node)
            common.append_node("./policy_lowest_lvl", policy_lowest, policy_node)
    elif cpu_has_eist:
        common.append_node(f"/acrn-config/hv/cpufreq/interface", "CPUFREQ_INTERFACE_ACPI", allocation_etree)
        p_count = board_cfg_lib.get_p_state_count()
        print(p_count)
        if p_count != 0:            
            for cpu in cpus:
                cpu_id = common.get_node("./cpu_id/text()", cpu)
                # P0 is the highest stat
                if cpu_id in rtvm_cpus:
                    # for CPUs in RTVM, fix to base performance
                    if cpu_has_turbo:
                        policy_highest = 1
                        policy_guaranteed = 1
                        policy_lowest = 1
                    else:
                        policy_highest = 0
                        policy_guaranteed = 0
                        policy_lowest = 0
                else:
                    if cpu_has_turbo:
                        policy_highest = 0
                        policy_guaranteed = 1
                        policy_lowest = p_count -1
                    else:
                        policy_highest = 0
                        policy_guaranteed = 0
                        policy_lowest = p_count -1
                policy_node = common.append_node(f"/acrn-config/hv/cpufreq/CPU[@id='{cpu_id}']/policy", None, allocation_etree)
                print(cpu_id, policy_guaranteed, policy_highest, policy_lowest)            
                common.append_node("./policy_guaranteed_lvl", str(policy_guaranteed), policy_node)
                common.append_node("./policy_highest_lvl", str(policy_highest), policy_node)
                common.append_node("./policy_lowest_lvl", str(policy_lowest), policy_node)
    else:
        common.append_node(f"/acrn-config/hv/cpufreq/interface", "CPUFREQ_INTERFACE_NONE", allocation_etree)


    # Let CPUs in the same frequency group have the same policy
    for alloc_cpu in allocation_etree.xpath("//cpufreq/CPU"):
        dependency_cpus = common.get_node("./freq_dependency/text()", alloc_cpu).split(" ")
        print("alloc_policy: dep cpu ", dependency_cpus)
        if common.get_node("./policy", alloc_cpu) != None:
            highest_lvl = int(common.get_node(".//policy_highest_lvl/text()", alloc_cpu))
            lowest_lvl = int(common.get_node(".//policy_lowest_lvl/text()", alloc_cpu))
            print(highest_lvl, ",", lowest_lvl, "->")
            for dep_cpu_id in dependency_cpus:
                dep_highest = int(common.get_node(f"//cpufreq/CPU[@id={dep_cpu_id}]//policy_highest_lvl/text()", allocation_etree))
                dep_lowest = int(common.get_node(f"//cpufreq/CPU[@id={dep_cpu_id}]//policy_lowest_lvl/text()", allocation_etree))
                if freq_interface == 'CPUFREQ_INTERFACE_HWP':
                    if highest_lvl > dep_highest:
                        highest_lvl = dep_highest
                    if lowest_lvl < dep_lowest:
                        lowest_lvl = dep_lowest
                else:
                    if highest_lvl < dep_highest:
                        highest_lvl = dep_highest
                    if lowest_lvl > dep_lowest:
                        lowest_lvl = dep_lowest
                    
            print(highest_lvl, ",", lowest_lvl)
            common.update_text("./policy/policy_highest_lvl", str(highest_lvl), alloc_cpu, True)
            common.update_text("./policy/policy_lowest_lvl", str(lowest_lvl), alloc_cpu, True)

# passthrough acpi p-state to vm when:
# 1. PT_ACPI_PSTATE = y
# 2. VM does not share CPU with none service vms
# 3. using ACPI p-state
def alloc_passthrough(board_etree, scenario_etree, allocation_etree):
    policy_nodes = allocation_etree.xpath("/acrn-config/hv/cpufreq/CPU/policy")
    config_passthrough = (common.get_node("//PT_ACPI_PSTATE/text()", scenario_etree) == 'y')
    all_cpu_ids = scenario_etree.xpath("//cpu_affinity//pcpu_id/text()")
    print("all_cpu_ids", all_cpu_ids)
    freq_interface = common.get_node("//CPUFREQ_INTERFACE/text()", scenario_etree)    
    vms = scenario_etree.xpath("//vm[load_order!='SERVICE_VM']")
    for vm in vms:
        vm_cpu_shared = 0
        vm_cpu_ids = vm.xpath(".//cpu_affinity//pcpu_id/text()")
        print(vm_cpu_ids)
        for cpu_id in vm_cpu_ids:
            print(cpu_id)
            if all_cpu_ids.count(cpu_id) > 1:
                print("cpu shared: ", cpu_id)
                vm_cpu_shared = 1
        print(vm_cpu_shared, config_passthrough, freq_interface)
        
        vm_id = common.get_node("./@id", vm)
        allocation_vm_node = common.get_node(f"/acrn-config/vm[@id='{vm_id}']", allocation_etree)
        if allocation_vm_node is None:
            allocation_vm_node = common.append_node("/acrn-config/vm", None, allocation_etree, id = vm_id)
        if vm_cpu_shared == 0 and config_passthrough and freq_interface == "CPUFREQ_INTERFACE_ACPI":
            print("pt acpi pstate ", vm_id)
            common.append_node("./vm_pt_acpi_pstate", 'y', allocation_vm_node)
            for cpu_id in vm.xpath(".//cpu_affinity//pcpu_id/text()"):
                print(cpu_id)
                policy_node = common.get_node(f"//cpufreq//CPU[@id='{cpu_id}']/policy", allocation_etree)
                common.append_node("./pcpu_pt_acpi_pstate", 'y', policy_node)
                print("passthrough", cpu_id)

def fn(board_etree, scenario_etree, allocation_etree):
    common.append_node("/acrn-config/hv/cpufreq", None, allocation_etree)
    alloc_dependency(board_etree, scenario_etree, allocation_etree)
    alloc_policy(board_etree, scenario_etree, allocation_etree)
    alloc_passthrough(board_etree, scenario_etree, allocation_etree)    
