#include "moar.h"

/* Adds a usage of an SSA value. */
void MVM_spesh_usages_add(MVMThreadContext *tc, MVMSpeshGraph *g, MVMSpeshFacts *facts, MVMSpeshIns *by) {
    MVMSpeshUseChainEntry *entry = MVM_spesh_alloc(tc, g, sizeof(MVMSpeshUseChainEntry));
    entry->user = by;
    entry->next = facts->usage.users;
    facts->usage.users = entry;
}
void MVM_spesh_usages_add_by_reg(MVMThreadContext *tc, MVMSpeshGraph *g, MVMSpeshOperand used, MVMSpeshIns *by) {
    MVM_spesh_usages_add(tc, g, MVM_spesh_get_facts(tc, g, used), by);
}

/* Removes a usage of an SSA value. */
void MVM_spesh_usages_delete(MVMThreadContext *tc, MVMSpeshGraph *g, MVMSpeshFacts *facts, MVMSpeshIns *by) {
    MVMSpeshUseChainEntry *cur = facts->usage.users;
    MVMSpeshUseChainEntry *prev = NULL;
    while (cur) {
        if (cur->user == by) {
            if (prev)
                prev->next = cur->next;
            else
                facts->usage.users = cur->next;
            return;
        }
        prev = cur;
        cur = cur->next;
    }
    MVM_oops(tc, "Spesh: instruction %s missing from define-use chain", by->info->name);
}
void MVM_spesh_usages_delete_by_reg(MVMThreadContext *tc, MVMSpeshGraph *g, MVMSpeshOperand used, MVMSpeshIns *by) {
    MVM_spesh_usages_delete(tc, g, MVM_spesh_get_facts(tc, g, used), by);
}

/* Marks that an SSA value is required for deopt purposes. */
void MVM_spesh_usages_add_for_deopt(MVMThreadContext *tc, MVMSpeshGraph *g, MVMSpeshFacts *facts) {
    facts->usage.deopt_required = 1;
}
void MVM_spesh_usages_add_for_deopt_by_reg(MVMThreadContext *tc, MVMSpeshGraph *g, MVMSpeshOperand used) {
    MVM_spesh_usages_add_for_deopt(tc, g, MVM_spesh_get_facts(tc, g, used));
}

/* Marks that an SSA value is required for exception handling purposes. */
void MVM_spesh_usages_add_for_handler(MVMThreadContext *tc, MVMSpeshGraph *g, MVMSpeshFacts *facts) {
    facts->usage.handler_required = 1;
}
void MVM_spesh_usages_add_for_handler_by_reg(MVMThreadContext *tc, MVMSpeshGraph *g, MVMSpeshOperand used) {
    MVM_spesh_usages_add_for_handler(tc, g, MVM_spesh_get_facts(tc, g, used));
}

/* Checks if the value is used, either by another instruction in the graph or
 * by being needed for deopt. */
MVMuint32 MVM_spesh_usages_is_used(MVMThreadContext *tc, MVMSpeshGraph *g, MVMSpeshOperand check) {
    MVMSpeshFacts *facts = MVM_spesh_get_facts(tc, g, check);
    return facts->usage.deopt_required || facts->usage.handler_required || facts->usage.users;
}

/* Checks if the value is used due to being required for deopt. */
MVMuint32 MVM_spesh_usages_is_used_by_deopt(MVMThreadContext *tc, MVMSpeshGraph *g, MVMSpeshOperand check) {
    MVMSpeshFacts *facts = MVM_spesh_get_facts(tc, g, check);
    return facts->usage.deopt_required;
}

/* Checks if the value is used due to being required for exception handling. */
MVMuint32 MVM_spesh_usages_is_used_by_handler(MVMThreadContext *tc, MVMSpeshGraph *g, MVMSpeshOperand check) {
    MVMSpeshFacts *facts = MVM_spesh_get_facts(tc, g, check);
    return facts->usage.handler_required;
}

/* Checks if there is precisely one known non-deopt user of the value. */
MVMuint32 MVM_spesh_usages_used_once(MVMThreadContext *tc, MVMSpeshGraph *g, MVMSpeshOperand check) {
    MVMSpeshFacts *facts = MVM_spesh_get_facts(tc, g, check);
    return !facts->usage.deopt_required && !facts->usage.handler_required &&
        facts->usage.users && !facts->usage.users->next;
}

/* Gets the count of usages, excluding use for deopt or handler purposes. */
MVMuint32 MVM_spesh_usages_count(MVMThreadContext *tc, MVMSpeshGraph *g, MVMSpeshOperand check) {
    MVMuint32 count = 0;
    MVMSpeshUseChainEntry *cur = MVM_spesh_get_facts(tc, g, check)->usage.users;
    while (cur) {
        count++;
        cur = cur->next;
    }
    return count;
}

#if MVM_SPESH_CHECK_DU
/* Check the DU chains of a graph are well formed. */
void MVM_spesh_usages_check(MVMThreadContext *tc, MVMSpeshGraph *g) {
    MVMSpeshBB *cur_bb = g->entry;
    while (cur_bb) {
        MVMSpeshIns *cur_ins = cur_bb->first_ins;
        while (cur_ins) {
            MVMuint8 is_phi = cur_ins->info->opcode == MVM_SSA_PHI;
            MVMuint8 i;
            for (i = 0; i < cur_ins->info->num_operands; i++) {
                if ((is_phi && i > 0)
                        || (!is_phi && (cur_ins->info->operands[i] & MVM_operand_rw_mask) == MVM_operand_read_reg)) {
                    /* It's a read. */
                    MVMSpeshFacts *facts = &(g->facts[cur_ins->operands[i].reg.orig][cur_ins->operands[i].reg.i]);
                    MVMSpeshUseChainEntry *use_entry = facts->usage.users;
                    MVMuint32 found = 0;
                    while (use_entry) {
                        if (use_entry->user == cur_ins) {
                            found = 1;
                            break;
                        }
                        use_entry = use_entry->next;
                    }
                    if (!found)
                        MVM_oops(tc, "Malformed DU chain: reader %s of %d(%d) in BB %d missing\n%s",
                            is_phi ? "PHI" : cur_ins->info->name,
                            cur_ins->operands[i].reg.orig, cur_ins->operands[i].reg.i,
                            cur_bb->idx,
                            MVM_spesh_dump(tc, g));
                }
                else if ((is_phi && i == 0)
                       || (!is_phi && (cur_ins->info->operands[i] & MVM_operand_rw_mask) == MVM_operand_write_reg)) {
                    /* It's a write. Check the writer is this instruction. */
                    MVMSpeshFacts *facts = &(g->facts[cur_ins->operands[i].reg.orig][cur_ins->operands[i].reg.i]);
                    if (facts->writer != cur_ins)
                        MVM_oops(tc, "Malformed DU chain: writer %s of %d(%d) in BB %d is incorrect\n%s",
                            is_phi ? "PHI" : cur_ins->info->name,
                            cur_ins->operands[i].reg.orig, cur_ins->operands[i].reg.i,
                            cur_bb->idx,
                            MVM_spesh_dump(tc, g));
                }
            }
            cur_ins = cur_ins->next;
        }
        cur_bb = cur_bb->linear_next;
    }
}
#endif