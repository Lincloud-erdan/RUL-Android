//
// Created by hangshu on 22-7-4.
//
#define DEBUG

#include "codegen.hh"
#include <set>
#include <algorithm>
#include "allocRegs.hh"

int GR::reg_num = 16;
int FR::reg_num = 32;
int bbNameCnt = 0;
std::map<std::string, std::string> bbNameMapping;
std::map<std::string, std::string> stringConstMapping;
int stringConstCnt = 0;


std::string Codegen::getFloatAddr(float x) {
    if (floatConstMapping.count(x) == 0) {
        floatConstMapping[x] = ".f" + std::to_string(floatConstCnt++);
    }
    return floatConstMapping[x];
}

bool is_legal_load_store_offset(int32_t offset) {
    return offset >= -4095 && offset <= 4095;
}

bool is_legal_immediate(int32_t value) {
    uint32_t u = static_cast<uint32_t>(value);
    if (u <= 0xffu) return true;
    for (int i = 1; i < 16; ++i) {
        uint32_t cur = (u << (2 * i)) | (u >> (32 - 2 * i));
        if (cur <= 0xffu) return true;
    }
    return false;
}

std::string getBBName(std::string name) {
    if (bbNameMapping.count(name) == 0) {
        bbNameMapping[name] = ".L" + std::to_string(bbNameCnt++);
    }
    return bbNameMapping[name];
}

std::vector<Instr *> setIntValue(GR target, uint32_t value) {
    if (0 <= value && value <= 65535) {
        return {new MovImm(target, value)};
    } else if (-255 <= value && value <= 0) {
        return {new MvnImm(target, -value - 1)};
    } else {
        int imm_low = value & ((1 << 16) - 1);
        int imm_high = value >> 16;
        return {new MoveW(target, imm_low),
                new MoveT(target, imm_high)};
    }
}

std::vector<Instr *> setFloatValue(GR target, float value) {
    union {
        float floatVal;
        uint32_t intVal;
    } data;
    data.floatVal = value;
    return setIntValue(target, data.intVal);
}

uint32_t floatToInt(float value) {
    union {
        float floatVal;
        uint32_t intVal;
    } data;
    data.floatVal = value;
    return data.intVal;
}
GR Codegen::getConstantGR(int x, BasicBlock* block, std::vector<Instr*>& vec) {
    if (!constantIntMapping[block].count(x)) {
        GR gr = GR::allocateReg();
        for (Instr *instr: setIntValue(gr, x)) {
            vec.push_back(instr);
        }
        constantIntMapping[block][x] = gr;
    }
    return constantIntMapping[block][x];
}
void Codegen::generateFloatConst() {
    for (std::pair<float, std::string> pair: floatConstMapping) {
        out << ".align\n";
        out << pair.second << ":\n";
        out << "\t.4byte " << floatToInt(pair.first) << "\n";
    }
}

void Codegen::generateMemset() {
    out << ".memset:\n";
    out << "\tpush {r4}\n";
    out << "\tmov r2,#0\n";
    out << "\tmov r3,#0\n";
    out << "\tmov r4,#8\n";
    out << ".memset8:\n";
    out << "\tsub r1,r1,#8\n";
    out << "\tcmp r1,#0\n";
    out << "\tblt .memset4\n";
    out << "\tstrd r2,r3,[r0],r4\n";
    out << "\tbne .memset8\n";;
    out << "\tb .memset_end\n";
    out << ".memset4:\n";
    out << "\tstr r2,[r0],#4\n";
    out << ".memset_end:\n";
    out << "\tpop {r4}\n";
    out << "\tbx lr\n";
}

void Codegen::generateProgramCode() {
    out << ".arch armv7ve\n";
    out << ".arm\n";
    out << ".fpu neon\n";
    out << ".text\n";
    out << ".global main\n";
    generateGlobalCode();
    out << ".section .text\n";
    generateMemset();
    int removeCnt = 0;
    Function *entry_func = new Function(".init", new Type(TypeID::VOID));
    entry_func->pushBB(irVisitor.entry);
    irVisitor.functions.push_back(entry_func);
    std::map<Function *, std::set<GR>> usedGRMapping;
    std::map<Function *, std::set<FR>> usedFRMapping;
    std::map<Function *, int> spillCountMapping;
    std::map<Function *, int> surplyFor8AlignMapping;
    for (auto itt = irVisitor.functions.rbegin(); itt != irVisitor.functions.rend(); itt++) {
        Function *function = *itt;
        if (function->basicBlocks.empty()) continue;
        //cope with phi
        {
            for (BasicBlock *block: function->basicBlocks) {
                for (Instruction* ir:block->getIr()) {
                    if (typeid(*ir) == typeid(PhiIR)) {
                        PhiIR* phiIr = dynamic_cast<PhiIR*>(ir);
                        for (std::pair<BasicBlock*, Value*> pair:phiIr->params) {
                            BasicBlock* pre = pair.first;
                            Value* src = pair.second;
                            auto it = pre->getIr().end();
                            if (typeid(**it) == typeid(JumpIR)) {
                                pre->getIr().insert(it - 1,new MoveIR(phiIr->dst, src));
                            } else if (typeid(**it) == typeid(BranchIR)) {
                                pre->getIr().insert(it - 2,new MoveIR(phiIr->dst, src));
                            } else {
                                pre->getIr().push_back(new MoveIR(phiIr,src));
                            }
                        }
                    }
                }
            }
        }
        function->stackSize = translateFunction(function);
        ColoringAlloc coloringAlloc(function);
        int spill_size = coloringAlloc.run() * 4;
        spillCountMapping[function] = spill_size;
        function->stackSize += spill_size;
        std::set<GR> allUsedRegsGR{GR(14)};
        std::set<FR> allUsedRegsFR;
        std::set<GR> callerSave;
        for (BasicBlock *block: function->basicBlocks) {
            for (auto it = block->getInstrs().rbegin(); it != block->getInstrs().rend();) {
                Instr *instr = *it;
                instr->replace(coloringAlloc.getColorGR(), coloringAlloc.getColorFR());
                instr->replaceBBName(bbNameMapping);
                if (typeid(*instr) == typeid(MoveReg) && instr->getUseG()[0] == instr->getDefG()[0] && (dynamic_cast<MoveReg*>(instr)->asr == -1)||
                    typeid(*instr) == typeid(VMoveReg) && instr->getUseF()[0] == instr->getDefF()[0]) {
                    block->getInstrs().erase((++it).base());
                    removeCnt++;
                } else {
                    for (GR gr: instr->getDefG()) {
                        if (callee_save_regs.count(gr) != 0) {
                            allUsedRegsGR.insert(gr);
                        }
                        if (caller_save_regs.count(gr) != 0) {
                            callerSave.erase(gr);
                        }
                    }
                    for (GR gr: instr->getUseG()) {
                        if (caller_save_regs.count(gr) != 0) {
                            callerSave.insert(gr);
                        }
                    }
                    for (FR fr: instr->getDefF()) {
                        allUsedRegsFR.insert(fr);
                    }
                    if (typeid(*instr) == typeid(Bl)) {
//                        Push* pushInstr = dynamic_cast<Push*>(*(it+1));
//                        Pop* popInstr = dynamic_cast<Pop*>(*(it-1));
//                        if (pushInstr && popInstr) {
//                            pushInstr->addRegs(callerSave);
//                            popInstr->addRegs(callerSave);
//                        }
                    }
                    it++;
                }
            }
        }
        usedGRMapping[function] = allUsedRegsGR;
        allUsedRegsFR.erase(FR(0));
        usedFRMapping[function] = allUsedRegsFR;
        //make sp%8 == 0
        if ((function->stackSize + spill_size + allUsedRegsGR.size() * 4 + allUsedRegsFR.size() * 4) % 8 != 0) {
            surplyFor8AlignMapping[function] = 4;
            function->stackSize += 4;
        } else {
            surplyFor8AlignMapping[function] = 0;
        }
    }
    for (auto itt = irVisitor.functions.rbegin(); itt != irVisitor.functions.rend(); itt++) {
        Function *function = *itt;
        if (function->basicBlocks.empty()) continue;
        if (function->name != ".init") {
            function->basicBlocks.insert(function->basicBlocks.begin(), new NormalBlock(function->name));
            bbNameMapping[function->name] = function->name;
            if (function->name == "main") {
                function->basicBlocks[0]->getInstrs().insert(function->basicBlocks[0]->getInstrs().begin(),
                                                             new Bl(".init"));
            }
            //sub r11,sp, #size;
//            if (spillCountMapping[function] != 0) {
//                if (is_legal_load_store_offset(spillCountMapping[function]) &&
//                    is_legal_immediate(spillCountMapping[function])) {
//                    function->basicBlocks[0]->getInstrs().insert(function->basicBlocks[0]->getInstrs().begin(),
//                                                                 new GRegImmInstr(GRegImmInstr::Sub,GR(11),GR(13),spillCountMapping[function]));
//                } else {
//                    function->basicBlocks[0]->getInstrs().insert(function->basicBlocks[0]->getInstrs().begin(),
//                                                                 new GRegRegInstr(GRegRegInstr::Sub,GR(11),GR(13),GR(12)));
//                    std::vector<Instr *> vec = setIntValue(GR(12), spillCountMapping[function]);
//                    for (int i = vec.size() - 1; i >= 0; --i) {
//                        function->basicBlocks[0]->getInstrs().insert(function->basicBlocks[0]->getInstrs().begin(), vec[i]);
//                    }
//                }
//            }
            if (is_legal_immediate(function->stackSize) && is_legal_load_store_offset(function->stackSize)) {
                function->basicBlocks[0]->getInstrs().insert(function->basicBlocks[0]->getInstrs().begin(),
                                                             new GRegImmInstr(GRegImmInstr::Sub, GR(13), GR(13),
                                                                              function->stackSize));
            } else {
                function->basicBlocks[0]->getInstrs().insert(function->basicBlocks[0]->getInstrs().begin(),
                                                             new GRegRegInstr(GRegRegInstr::Sub, GR(13), GR(13),
                                                                              GR(12)));
                std::vector<Instr *> vec = setIntValue(GR(12), function->stackSize);
                for (int i = vec.size() - 1; i >= 0; --i) {
                    function->basicBlocks[0]->getInstrs().insert(function->basicBlocks[0]->getInstrs().begin(), vec[i]);
                }
                usedGRMapping[function].insert(GR(12));
            }
            if (usedFRMapping[function].size() <= 16) {
                function->basicBlocks[0]->getInstrs().insert(function->basicBlocks[0]->getInstrs().begin(),
                                                             new Vpush(usedFRMapping[function]));
            } else {
                std::set<FR> first, second;
                int cnt = 0;
                for (auto item: usedFRMapping[function]) {
                    if (cnt < 16) {
                        first.insert(item);
                    } else {
                        second.insert(item);
                    }
                    cnt++;
                }
                function->basicBlocks[0]->getInstrs().insert(function->basicBlocks[0]->getInstrs().begin(),
                                                             new Vpush(first));
                function->basicBlocks[0]->getInstrs().insert(function->basicBlocks[0]->getInstrs().begin(),
                                                             new Vpush(second));
            }
            function->basicBlocks[0]->getInstrs().insert(function->basicBlocks[0]->getInstrs().begin(),
                                                         new Push(usedGRMapping[function]));
            //simple way
        } else {
            function->basicBlocks[0]->getInstrs().push_back(new Bx());
        }
        for (BasicBlock *block: function->basicBlocks) {
            for (auto it = block->getInstrs().begin(); it != block->getInstrs().end();) {
                Instr *instr = *it;
                if (typeid(*instr) == typeid(Load)) {
                    Load *load = dynamic_cast<Load *>(instr);
                    if (load->offset < 0) {
                        load->offset = -load->offset + usedGRMapping[function].size() * 4 +
                                       usedFRMapping[function].size() * 4 + spillCountMapping[function] +
                                       surplyFor8AlignMapping[function];
                    }
                    if (!is_legal_immediate(load->offset) || !is_legal_load_store_offset(load->offset)) {
                        it = block->getInstrs().erase(it);
                        it = block->getInstrs().insert(it, new Load(load->dst, GR(12), 0));
                        it = block->getInstrs().insert(it,
                                                       new GRegRegInstr(GRegRegInstr::Add, GR(12), GR(12), load->base));
                        std::vector<Instr *> vv = setIntValue(GR(12), load->offset);
                        for (auto item = vv.rbegin(); item != vv.rend(); item++) {
                            it = block->getInstrs().insert(it, *item);
                        }
                        it = it + 1 + vv.size();
                    }
                }
                if (typeid(*instr) == typeid(Store)) {
                    Store *store = dynamic_cast<Store *>(instr);
                    if (store->offset < 0) {
                        store->offset = -store->offset + usedGRMapping[function].size() * 4 +
                                        usedFRMapping[function].size() * 4 + spillCountMapping[function] +
                                        surplyFor8AlignMapping[function];
                    }
                    if (!is_legal_immediate(store->offset) || !is_legal_load_store_offset(store->offset)) {
                        it = block->getInstrs().erase(it);
                        it = block->getInstrs().insert(it, new Store(store->src, GR(12), 0));
                        it = block->getInstrs().insert(it, new GRegRegInstr(GRegRegInstr::Add, GR(12), GR(12),
                                                                            store->base));
                        std::vector<Instr *> vv = setIntValue(GR(12), store->offset);
                        for (auto item = vv.rbegin(); item != vv.rend(); item++) {
                            it = block->getInstrs().insert(it, *item);
                        }
                        it = it + 1 + vv.size();
                    }
                }
                if (typeid(*instr) == typeid(VLoad)) {
                    VLoad *vload = dynamic_cast<VLoad *>(instr);
                    if (vload->offset < 0) {
                        vload->offset = -vload->offset + usedGRMapping[function].size() * 4 +
                                        usedFRMapping[function].size() * 4 + spillCountMapping[function] +
                                        surplyFor8AlignMapping[function];
                    }
                    if (!is_legal_immediate(vload->offset) || !is_legal_load_store_offset(vload->offset)) {
                        it = block->getInstrs().erase(it);
                        it = block->getInstrs().insert(it, new VLoad(vload->dst, GR(12), 0));
                        it = block->getInstrs().insert(it, new GRegRegInstr(GRegRegInstr::Add, GR(12), GR(12),
                                                                            vload->base));
                        std::vector<Instr *> vv = setIntValue(GR(12), vload->offset);
                        for (auto item = vv.rbegin(); item != vv.rend(); item++) {
                            it = block->getInstrs().insert(it, *item);
                        }
                        it = it + 1 + vv.size();
                    }
                }
                if (typeid(*instr) == typeid(VStore)) {
                    VStore *vstore = dynamic_cast<VStore *>(instr);
                    if (vstore->offset < 0) {
                        vstore->offset = -vstore->offset + usedGRMapping[function].size() * 4 +
                                         usedFRMapping[function].size() * 4 + spillCountMapping[function] +
                                         surplyFor8AlignMapping[function];
                    }
                    if (!is_legal_immediate(vstore->offset) || !is_legal_load_store_offset(vstore->offset)) {
                        it = block->getInstrs().erase(it);
                        it = block->getInstrs().insert(it, new VStore(vstore->src, GR(12), 0));
                        it = block->getInstrs().insert(it,
                                                       new GRegRegInstr(GRegRegInstr::Add, GR(12), GR(12),
                                                                        vstore->base));
                        std::vector<Instr *> vv = setIntValue(GR(12), vstore->offset);
                        for (auto item = vv.rbegin(); item != vv.rend(); item++) {
                            it = block->getInstrs().insert(it, *item);
                        }
                        it = it + 1 + vv.size();
                    }
                }
//                if (typeid(*instr) == typeid(MoveReg)) {
//                    MoveReg* moveReg = dynamic_cast<MoveReg*>(instr);
//                    //mov sp,r11;
//                    if (moveReg->getUseG()[0] == GR(11)) {
//                        block->getInstrs().erase(it);
//                        int cnt = 0;
//                        if (spillCountMapping[function] != 0) {
//                            if (is_legal_immediate(spillCountMapping[function]) && is_legal_load_store_offset(spillCountMapping[function])) {
//                                block->getInstrs().insert(it,new GRegImmInstr(GRegImmInstr::Sub, GR(13),GR(13),spillCountMapping[function]));
//                                cnt++;
//                            } else {
//                                block->getInstrs().insert(it,new GRegRegInstr(GRegRegInstr::Sub,GR(13),GR(13),GR(12)));
//                                std::vector<Instr*> vv = setIntValue(GR(12), spillCountMapping[function]);
//                                for (auto item = vv.rbegin();item != vv.rend();it++) {
//                                    block->getInstrs().insert(it,*item);
//                                }
//                                cnt = cnt + 1 + vv.size();
//                            }
//                        }
//                        it = it + cnt - 1;
//                    }
//                    if (moveReg->getDefG()[0] == GR(11)) {
//                        block->getInstrs().erase(it);
//                        int cnt = 0;
//                        if (spillCountMapping[function] != 0) {
//                            if (is_legal_immediate(spillCountMapping[function]) && is_legal_load_store_offset(spillCountMapping[function])) {
//                                block->getInstrs().insert(it,new GRegImmInstr(GRegImmInstr::Add, GR(13),GR(13),spillCountMapping[function]));
//                                cnt++;
//                            } else {
//                                block->getInstrs().insert(it,new GRegRegInstr(GRegRegInstr::Add,GR(13),GR(13),GR(12)));
//                                std::vector<Instr*> vv = setIntValue(GR(12), spillCountMapping[function]);
//                                for (auto item = vv.rbegin();item != vv.rend();it++) {
//                                    block->getInstrs().insert(it,*item);
//                                }
//                                cnt = cnt + 1 + vv.size();
//                            }
//                        }
//                        it = it + cnt - 1;
//                    }
//                }

                if (typeid(*instr) == typeid(Ret)) {
                    block->getInstrs().erase(it);
                    if (is_legal_immediate(function->stackSize) && is_legal_load_store_offset(function->stackSize)) {
                        if (function->stackSize != 0)
                            block->getInstrs().push_back(
                                    new GRegImmInstr(GRegImmInstr::Add, GR(13), GR(13), function->stackSize));
                    } else {
                        std::vector<Instr *> vec = setIntValue(GR(12), function->stackSize);
                        for (int i = 0; i < vec.size(); i++) {
                            block->getInstrs().push_back(vec[i]);
                        }
                        block->getInstrs().push_back(new GRegRegInstr(GRegRegInstr::Add, GR(13), GR(13), GR(12)));
                    }
                    std::set<GR> setGR = usedGRMapping[function];
                    setGR.erase(GR(14));
                    setGR.insert(GR(15));
                    if (!usedFRMapping[function].empty()) {
                        if (usedFRMapping[function].size() <= 16) {
                            block->getInstrs().push_back(new Vpop(usedFRMapping[function]));
                        } else {
                            std::set<FR> first, second;
                            int cnt = 0;
                            for (auto item: usedFRMapping[function]) {
                                if (cnt < 16) {
                                    first.insert(item);
                                } else {
                                    second.insert(item);
                                }
                                cnt++;
                            }
                            if (!first.empty()) {
                                block->getInstrs().push_back(new Vpop(first));
                            }
                            if (!second.empty()) {
                                block->getInstrs().push_back(new Vpop(second));
                            }
                        }
                    }
                    if (!setGR.empty()) {
                        block->getInstrs().push_back(new Pop(setGR));
                    }
                    break;
                } else if (typeid(*instr) == typeid(Push)) {
                    Push *pushInstr = dynamic_cast<Push *>(instr);
                    if (pushInstr->regs.empty()) {
                        block->getInstrs().erase(it);
                    } else {
                        it++;
                    }
                } else if (typeid(*instr) == typeid(Pop)) {
                    Pop *pushInstr = dynamic_cast<Pop *>(instr);
                    if (pushInstr->regs.empty()) {
                        block->getInstrs().erase(it);
                    } else {
                        it++;
                    }
                } else if (typeid(*instr) == typeid(Vpop)) {
                    Vpop *pushInstr = dynamic_cast<Vpop *>(instr);
                    if (pushInstr->regs.empty()) {
                        block->getInstrs().erase(it);
                    } else {
                        it++;
                    }
                } else if (typeid(*instr) == typeid(Vpush)) {
                    Vpush *pushInstr = dynamic_cast<Vpush *>(instr);
                    if (pushInstr->regs.empty()) {
                        block->getInstrs().erase(it);
                    } else {
                        it++;
                    }
                } else if (typeid(*instr) == typeid(GRegImmInstr)) {
                    GRegImmInstr *g = dynamic_cast<GRegImmInstr *>(instr);
                    if ((g->op == GRegRegInstr::Add && g->src2 == 0 ||
                         g->op == GRegRegInstr::Sub && g->src2 == 0) && (g->dst == g->src1)) {
                        block->getInstrs().erase(it);
                    } else {
                        it++;
                    }
                } else {
                    it++;
                }
            }
        }
    }
    for (auto itt = irVisitor.functions.rbegin(); itt != irVisitor.functions.rend(); itt++) {
        Function *function = *itt;
        if (function->basicBlocks.empty()) continue;
        if (function->name == ".init") {
            out << function->name << ":\n";
        } else {
            comment("spilled Size: " + std::to_string(spillCountMapping[function]));
            comment("stack Size: " + std::to_string(function->stackSize));
        }
        for (BasicBlock *block: function->basicBlocks) {
            out << getBBName(block->name) << ":\n";
            for (auto it = block->getInstrs().begin(); it != block->getInstrs().end();) {
                Instr *instr = *it;
                it++;
                out << "\t";
                instr->print(out);
            }
        }
    }
    generateFloatConst();
}

int Codegen::translateFunction(Function *function) {
    stackMapping.clear();
    int stackSize = 0;
    GR::reg_num = 16;
    FR::reg_num = 32;
    for (BasicBlock *bb: function->basicBlocks) {
        getBBName(bb->name);
        for (Instruction *ir: bb->ir) {
            if (typeid(*ir) == typeid(CallIR)) {
                int size = 0;
                CallIR *callIr = dynamic_cast<CallIR *>(ir);
                int gr_cnt = 0;
                int fr_cnt = 0;
                for (TempVal v: callIr->args) {
                    if (!v.isFloat()) {
                        if (gr_cnt >= 4) {
                            size += 4;
                        }
                        gr_cnt++;
                    } else {
                        if (fr_cnt >= 32) {
                            size += 4;
                        }
                        fr_cnt++;
                    }
                }
                stackSize = stackSize > size ? stackSize : size;
            }
        }
    }
    for (BasicBlock *bb: function->basicBlocks) {
        for (Instruction *ir: bb->ir) {
            if (dynamic_cast<AllocIR *>(ir)) {
                AllocIR *allocIr = dynamic_cast<AllocIR *>(ir);
                stackMapping[allocIr->v] = stackSize;
                stackSize += allocIr->arrayLen * 4;
            }
        }
    }
    {
        int gr_cnt = 0;
        int fr_cnt = 0;
        int cnt = 0;
        for (int i = 0; i < function->params.size(); i++) {
            if (!function->params[i]->getType()->isFloat()) {
                if (gr_cnt < 4) {
                    gRegMapping[function->params[i]] = GR(gr_cnt);
                } else {
                    //push {}  -> stackSize + cnt * 4 + pushSize
                    // eg :push {r4,r5} int f(int a,int b,int c,int d,int e)
                    // e -> [sp, # (16 + 8)]
                    stackMapping[function->params[i]] = -(stackSize + cnt * 4);
                    cnt++;
                }
                gr_cnt++;
            } else {
                if (fr_cnt < 32) {
                    fRegMapping[function->params[i]] = FR(fr_cnt);
                } else {
                    stackMapping[function->params[i]] = -(stackSize + cnt * 4);
                    cnt++;
                }
                fr_cnt++;
            }
        }
    }
    for (BasicBlock *bb: function->basicBlocks) {
        for (Instruction *ir: bb->ir) {
            std::vector<Instr *> vec = translateInstr(ir,bb);
            for (int i = 0; i < vec.size(); ++i) {
                bb->pushInstr(vec[i]);
            }
        }
    }
    return stackSize;
}

std::vector<Instr *> Codegen::translateInstr(Instruction *ir, BasicBlock* block) {
    if (typeid(*ir) == typeid(MoveIR)) {
        MoveIR* moveIr = dynamic_cast<MoveIR *>(ir);
        Value* dst = dynamic_cast<Value*>(moveIr->dst);
        TempVal* src = dynamic_cast<TempVal*>(moveIr->src);
        assert(dst != nullptr);
        assert(src != nullptr);
        std::vector<Instr*> vec;
        if (dst->getType()->isInt()) {
            if (!src->getVal()) {
                if (is_legal_immediate(src->getInt())) {
                    vec.push_back(new MovImm(getGR(dst), src->getInt()));
                } else {
                    getConstantGR(src->getInt(),block,vec);
                    vec.push_back(new MoveReg(getGR(dst), constantIntMapping[block][src->getInt()]));
                }
            } else {
                vec.push_back(new MoveReg(getGR(dst), getGR(src)));
            }
        } else {
            if (!src->getVal()) {
                FR d = FR::allocateReg();
                vec.push_back(new MoveWFromSymbol(GR(12), getFloatAddr(src->getFloat())));
                vec.push_back(new MoveTFromSymbol(GR(12), getFloatAddr(src->getFloat())));
                vec.push_back(new VLoad(d, GR(12), 0));
                vec.push_back(new VMoveReg(getFR(dst),d));
            } else {
                vec.push_back(new VMoveReg(getFR(dst), getFR(src)));
            }
        }
        return vec;
    }
    if (typeid(*ir) == typeid(AllocIIR)) {
        AllocIIR *allocIir = dynamic_cast<AllocIIR *>(ir);
        std::vector<Instr *> vec;
        if (allocIir->isArray) {
            if (is_legal_immediate(stackMapping[allocIir->v]) &&
                is_legal_load_store_offset(stackMapping[allocIir->v])) {
                vec.push_back(new GRegImmInstr(GRegImmInstr::Add, GR(0), GR(13), stackMapping[allocIir->v]));
            } else {
                std::vector<Instr *> v = setIntValue(GR(12), stackMapping[allocIir->v]);
                for (Instr *instr: v) {
                    vec.push_back(instr);
                }
                vec.push_back(new GRegRegInstr(GRegRegInstr::Add, GR(0), GR(13), GR(12)));
            }
            for (Instr *instr: setIntValue(GR(1), allocIir->arrayLen * 4)) {
                vec.push_back(instr);
            }
            vec.push_back(new Bl(".memset"));
        }
        return vec;
    }
    if (typeid(*ir) == typeid(AllocFIR)) {
        AllocFIR *allocIir = dynamic_cast<AllocFIR *>(ir);
        std::vector<Instr *> vec;
        if (allocIir->isArray) {
            if (is_legal_immediate(stackMapping[allocIir->v]) &&
                is_legal_load_store_offset(stackMapping[allocIir->v])) {
                vec.push_back(new GRegImmInstr(GRegImmInstr::Add, GR(0), GR(13), stackMapping[allocIir->v]));
            } else {
                std::vector<Instr *> v = setIntValue(GR(12), stackMapping[allocIir->v]);
                for (Instr *instr: v) {
                    vec.push_back(instr);
                }
                vec.push_back(new GRegRegInstr(GRegRegInstr::Add, GR(0), GR(13), GR(12)));
            }
            for (Instr *instr: setIntValue(GR(1), allocIir->arrayLen * 4)) {
                vec.push_back(instr);
            }
            vec.push_back(new Bl(".memset"));
        }
        return vec;
    }
    if (typeid(*ir) == typeid(GEPIR)) {
        GEPIR *gepIr = dynamic_cast<GEPIR *>(ir);
        std::vector<Instr *> vec;
        if (gepIr->v2->is_Global()) {
            GR dst = getGR(gepIr->v1);
            vec.push_back(new MoveWFromSymbol(dst, gepIr->v2->getName()));
            vec.push_back(new MoveTFromSymbol(dst, gepIr->v2->getName()));
            if (!gepIr->v3) {
                if (is_legal_load_store_offset(4 * gepIr->arrayLen) && is_legal_immediate(4 * gepIr->arrayLen)) {
                    vec.push_back(new GRegImmInstr(GRegImmInstr::Add, dst, dst, 4 * gepIr->arrayLen));
                } else {
                    std::vector<Instr *> v = setIntValue(GR(12), 4 * gepIr->arrayLen);
                    for (Instr *instr: v) {
                        vec.push_back(instr);
                    }
                    vec.push_back(new GRegRegInstr(GRegRegInstr::Add, dst, dst, GR(12)));
                }
            } else {
                Value *val = new VarValue();
                GR mul1 = getGR(val);
                vec.push_back(new MovImm(mul1, 4));
                vec.push_back(new MLA(dst, mul1, getGR(gepIr->v3), dst));
            }
        } else {
            if (!gepIr->v3) {
                if (stackMapping.count(gepIr->v2) == 0) {
                    GR dst = getGR(gepIr->v1);
                    if (is_legal_load_store_offset(gepIr->arrayLen * 4) && is_legal_immediate(4 * gepIr->arrayLen)) {
                        vec.push_back(new GRegImmInstr(GRegImmInstr::Add, dst, getGR(gepIr->v2), gepIr->arrayLen * 4));
                    } else {
                        std::vector<Instr *> v = setIntValue(GR(12), 4 * gepIr->arrayLen);
                        for (Instr *instr: v) {
                            vec.push_back(instr);
                        }
                        vec.push_back(new GRegRegInstr(GRegRegInstr::Add, dst, getGR(gepIr->v2), GR(12)));
                    }
                } else {
                    stackMapping[gepIr->v1] = stackMapping[gepIr->v2] + 4 * gepIr->arrayLen;
                }
            } else {
                //v3 * 4 + v2
                GR dst = getGR(gepIr->v1);
                Value *val2 = new VarValue();
                GR mul1 = getGR(val2);
                vec.push_back(new MovImm(mul1, 4));
                if (stackMapping.count(gepIr->v2) != 0) {
                    if (is_legal_load_store_offset(stackMapping[gepIr->v2]) &&
                        is_legal_immediate(stackMapping[gepIr->v2])) {
                        vec.push_back(
                                new GRegImmInstr(GRegImmInstr::Add, getGR(gepIr->v2), GR(13), stackMapping[gepIr->v2]));
                    } else {
                        std::vector<Instr *> v = setIntValue(GR(12), stackMapping[gepIr->v2]);
                        for (Instr *instr: v) {
                            vec.push_back(instr);
                        }
                        vec.push_back(new GRegRegInstr(GRegRegInstr::Add, getGR(gepIr->v2), GR(13), GR(12)));
                    }
                    vec.push_back(new MLA(dst, mul1, getGR(gepIr->v3), getGR(gepIr->v2)));

                } else {
                    vec.push_back(new MLA(dst, mul1, getGR(gepIr->v3), getGR(gepIr->v2)));
                }
            }
        }
        return vec;
    }
    if (typeid(*ir) == typeid(LoadIIR)) {
        LoadIIR *loadIr = dynamic_cast<LoadIIR *>(ir);
        std::vector<Instr *> vec;
        GR dst = getGR(loadIr->v1);
        if (loadIr->v2->is_Global()) {
            Value *v = new VarValue();
            GR base = getGR(v);
            vec.push_back(new MoveWFromSymbol(base, loadIr->v2->getName()));
            vec.push_back(new MoveTFromSymbol(base, loadIr->v2->getName()));
            vec.push_back(new Load(dst, base, 0));
        } else {
            if (stackMapping.count(loadIr->v2) != 0) {
                if (is_legal_load_store_offset(stackMapping[loadIr->v2]) &&
                    is_legal_immediate(stackMapping[loadIr->v2])
                    || stackMapping[loadIr->v2] < 0) {
                    vec.push_back(new Load(dst, GR(13), stackMapping[loadIr->v2]));
                } else {
                    std::vector<Instr *> v = setIntValue(GR(12), stackMapping[loadIr->v2]);
                    for (Instr *instr: v) {
                        vec.push_back(instr);
                    }
                    vec.push_back(new GRegRegInstr(GRegRegInstr::Add, GR(12), GR(12), GR(13)));
                    vec.push_back(new Load(dst, GR(12), 0));
                }
            } else {
                vec.push_back(new Load(dst, getGR(loadIr->v2), 0));
            }
        }
        return vec;
    }
    if (typeid(*ir) == typeid(LoadFIR)) {
        LoadFIR *loadIr = dynamic_cast<LoadFIR *>(ir);
        std::vector<Instr *> vec;
        FR dst = getFR(loadIr->v1);
        if (loadIr->v2->is_Global()) {
            Value *v = new VarValue();
            GR base = getGR(v);
            vec.push_back(new MoveWFromSymbol(base, loadIr->v2->getName()));
            vec.push_back(new MoveTFromSymbol(base, loadIr->v2->getName()));
            vec.push_back(new VLoad(dst, base, 0));
        } else {
            if (stackMapping.count(loadIr->v2) != 0) {
                if (is_legal_load_store_offset(stackMapping[loadIr->v2]) &&
                    is_legal_immediate(stackMapping[loadIr->v2])
                    || stackMapping[loadIr->v2] < 0) {
                    vec.push_back(new VLoad(dst, GR(13), stackMapping[loadIr->v2]));
                } else {
                    std::vector<Instr *> v = setIntValue(GR(12), stackMapping[loadIr->v2]);
                    for (Instr *instr: v) {
                        vec.push_back(instr);
                    }
                    vec.push_back(new GRegRegInstr(GRegRegInstr::Add, GR(12), GR(12), GR(13)));
                    vec.push_back(new VLoad(dst, GR(12), 0));
                }
            } else {
                vec.push_back(new VLoad(dst, getGR(loadIr->v2), 0));
            }
        }
        return vec;
    }
    if (typeid(*ir) == typeid(StoreIIR)) {
        StoreIIR *storeIr = dynamic_cast<StoreIIR *>(ir);
        std::vector<Instr *> vec;
        GR src;
        if (!storeIr->src.getVal()) {
            Value *val = new VarValue();
            storeIr->src.setVal(val);
            src = getGR(val);
            std::vector<Instr *> v = setIntValue(src, storeIr->src.getInt());
            for (Instr *instr: v) {
                vec.push_back(instr);
            }
        } else {
            if (gRegMapping.count(storeIr->src.getVal()) != 0) {
                src = getGR(storeIr->src.getVal());
            } else {
                stackMapping[storeIr->dst] = stackMapping[storeIr->src.getVal()];
                return vec;
            }
        }
        if (storeIr->dst->is_Global()) {
            Value *v = new VarValue();
            GR base = getGR(v);
            vec.push_back(new MoveWFromSymbol(base, storeIr->dst->getName()));
            vec.push_back(new MoveTFromSymbol(base, storeIr->dst->getName()));
            vec.push_back(new Store(src, base, 0));
        } else {
            if (stackMapping.count(storeIr->dst) != 0) {
                if (is_legal_load_store_offset(stackMapping[storeIr->dst]) &&
                    is_legal_immediate(stackMapping[storeIr->dst])
                    || stackMapping[storeIr->dst] < 0) {
                    vec.push_back(new Store(src, GR(13), stackMapping[storeIr->dst]));
                } else {
                    std::vector<Instr *> v = setIntValue(GR(12), stackMapping[storeIr->dst]);
                    for (Instr *instr: v) {
                        vec.push_back(instr);
                    }
                    vec.push_back(new GRegRegInstr(GRegRegInstr::Add, GR(12), GR(12), GR(13)));
                    vec.push_back(new Store(src, GR(12), 0));
                }
            } else {
                vec.push_back(new Store(src, getGR(storeIr->dst), 0));
            }
        }
        return vec;
    }
    if (typeid(*ir) == typeid(StoreFIR)) {
        StoreFIR *storeIr = dynamic_cast<StoreFIR *>(ir);
        std::vector<Instr *> vec;
        FR src;
        if (!storeIr->src.getVal()) {
            Value *val = new VarValue();
            storeIr->src.setVal(val);
            src = getFR(val);
            vec.push_back(new MoveWFromSymbol(GR(12), getFloatAddr(storeIr->src.getFloat())));
            vec.push_back(new MoveTFromSymbol(GR(12), getFloatAddr(storeIr->src.getFloat())));
            vec.push_back(new VLoad(src, GR(12), 0));
        } else {
            if (fRegMapping.count(storeIr->src.getVal()) != 0) {
                src = getFR(storeIr->src.getVal());
            } else {
                stackMapping[storeIr->dst] = stackMapping[storeIr->src.getVal()];
                return vec;
            }
        }
        if (storeIr->dst->is_Global()) {
            Value *v = new VarValue();
            GR base = getGR(v);
            vec.push_back(new MoveWFromSymbol(base, storeIr->dst->getName()));
            vec.push_back(new MoveTFromSymbol(base, storeIr->dst->getName()));
            vec.push_back(new VStore(src, base, 0));
        } else {
            if (stackMapping.count(storeIr->dst) != 0) {
                if (is_legal_load_store_offset(stackMapping[storeIr->dst]) &&
                    is_legal_immediate(stackMapping[storeIr->dst])
                    || stackMapping[storeIr->dst] < 0) {
                    vec.push_back(new VStore(src, GR(13), stackMapping[storeIr->dst]));
                } else {
                    std::vector<Instr *> v = setIntValue(GR(12), stackMapping[storeIr->dst]);
                    for (Instr *instr: v) {
                        vec.push_back(instr);
                    }
                    vec.push_back(new GRegRegInstr(GRegRegInstr::Add, GR(12), GR(12), GR(13)));
                    vec.push_back(new VStore(src, GR(12), 0));
                }
            } else {
                vec.push_back(new VStore(src, getGR(storeIr->dst), 0));
            }
        }
        return vec;
    }
    if (typeid(*ir) == typeid(UnaryIR)) {
        std::vector<Instr *> vec;
        UnaryIR *unaryIr = dynamic_cast<UnaryIR *>(ir);
        if (unaryIr->res.isInt()) {
            GR src = gRegMapping[unaryIr->v.getVal()];
            gRegMapping[unaryIr->res.getVal()] = src;
            if (unaryIr->op == OP::NEG) {
                vec.push_back(new RsubImm(gRegMapping[unaryIr->res.getVal()], gRegMapping[unaryIr->res.getVal()], 0));
            } else {
                vec.push_back(new CmpImm(src, 0));
                vec.push_back(new MovImm(src, 0));
                vec.push_back(new MovImm(src, 1, EQU));
            }
        } else {
            FR src = fRegMapping[unaryIr->v.getVal()];
            if (unaryIr->op == OP::NEG) {
                fRegMapping[unaryIr->res.getVal()] = src;
                vec.push_back(new VNeg(src, src));
            } else {
                vec.push_back(new VCmpe(src, 0));
                vec.push_back(new VMrs());
                GR dst = getGR(unaryIr->res.getVal());
                vec.push_back(new MovImm(dst, 0));
                vec.push_back(new MovImm(dst, 1, EQU));
            }
        }
        return vec;
    }
    if (typeid(*ir) == typeid(CastInt2FloatIR)) {
        CastInt2FloatIR *ir2 = dynamic_cast<CastInt2FloatIR *>(ir);
        GR src = getGR(ir2->v2);
        FR dst = getFR(ir2->v1);
        return {new VMovFG(dst, src), new VcvtFS(dst, dst)};
    }
    if (typeid(*ir) == typeid(CastFloat2IntIR)) {
        CastFloat2IntIR *ir2 = dynamic_cast<CastFloat2IntIR *>(ir);
        FR src = getFR(ir2->v2);
        GR dst = getGR(ir2->v1);
        return {new VcvtSF(src, src), new VMovGF(dst, src)};
    }

    if (typeid(*ir) == typeid(AddIIR)) {
        std::vector<Instr*> vec;
        AddIIR *addIir = dynamic_cast<AddIIR*>(ir);
        if (!addIir->left.getVal() && addIir->right.getVal()) {
            TempVal temp = addIir->left;
            addIir->left = addIir->right;
            addIir->right = temp;
        }
        if (!addIir->right.getVal()) {
            if (is_legal_immediate(addIir->right.getInt())) {
                vec.push_back(new GRegImmInstr(GRegImmInstr::Add, getGR(addIir->res.getVal()), getGR(addIir->left.getVal()), addIir->right.getInt()));
            } else if (is_legal_immediate(-addIir->right.getInt())){
                vec.push_back(new GRegImmInstr(GRegImmInstr::Sub, getGR(addIir->res.getVal()), getGR(addIir->left.getVal()), -addIir->right.getInt()));
            } else {
                getConstantGR(addIir->right.getInt(),block,vec);
                vec.push_back(new GRegRegInstr(GRegRegInstr::Add,
                                               getGR(addIir->res.getVal()),
                                               getGR(addIir->left.getVal()),
                                               constantIntMapping[block][addIir->right.getInt()]));
            }
        } else {
            vec.push_back(new GRegRegInstr(GRegRegInstr::Add, getGR(addIir->res.getVal()), getGR(addIir->left.getVal()),
                                           getGR(addIir->right.getVal())));
        }
        return vec;
    }
    if (typeid(*ir) == typeid(SubIIR)) {
        std::vector<Instr*> vec;
        SubIIR *subIir = dynamic_cast<SubIIR*>(ir);
        if (!subIir->right.getVal()) {
            if (is_legal_immediate(subIir->right.getInt())) {
                vec.push_back(new GRegImmInstr(GRegImmInstr::Sub, getGR(subIir->res.getVal()), getGR(subIir->left.getVal()), subIir->right.getInt()));
            } else if (is_legal_immediate(-subIir->right.getInt())){
                vec.push_back(new GRegImmInstr(GRegImmInstr::Add, getGR(subIir->res.getVal()), getGR(subIir->left.getVal()), -subIir->right.getInt()));
            } else {
                getConstantGR(subIir->right.getInt(),block,vec);
                vec.push_back(new GRegRegInstr(GRegRegInstr::Sub,
                                               getGR(subIir->res.getVal()),
                                               getGR(subIir->left.getVal()),
                                               constantIntMapping[block][subIir->right.getInt()]));
            }
        } else if (!subIir->left.getVal()) {
            if (is_legal_immediate(subIir->left.getInt())) {
                vec.push_back(new GRegImmInstr(GRegImmInstr::RSUB, getGR(subIir->res.getVal()), getGR(subIir->right.getVal()), subIir->left.getInt()));
            } else {
                getConstantGR(subIir->left.getInt(),block,vec);
                vec.push_back(new GRegRegInstr(GRegRegInstr::Sub,
                                               getGR(subIir->res.getVal()),
                                               constantIntMapping[block][subIir->left.getInt()],
                                               getGR(subIir->right.getVal())));
            }
        } else {
            vec.push_back(new GRegRegInstr(GRegRegInstr::Sub, getGR(subIir->res.getVal()), getGR(subIir->left.getVal()),
                                           getGR(subIir->right.getVal())));
        }
        return vec;
    }
    if (typeid(*ir) == typeid(MulIIR)) {
        std::vector<Instr*> vec;
        MulIIR* mulIir = dynamic_cast<MulIIR*>(ir);
        if (!mulIir->left.getVal() && mulIir->right.getVal()) {
            TempVal temp = mulIir->left;
            mulIir->left = mulIir->right;
            mulIir->right = temp;
        }
        if (!mulIir->right.getVal()) {
            int v = mulIir->right.getInt();
            bool flag = false;
            //add inst * 1
            {
                for (int lsl_1 = 0;lsl_1 < 32;lsl_1++) {
                    int x = 1;
                    int y;
                    y = (x + (x << lsl_1));
                    if (y == v) {
                        flag = true;
                        vec.push_back(new GRegRegInstr(GRegRegInstr::Add, getGR(mulIir->res.getVal()),
                                                       getGR(mulIir->left.getVal()),
                                                       getGR(mulIir->left.getVal()), GRegRegInstr::LSL,lsl_1));
                        break;
                    }
                }
            }
            //sub inst*1
            {
                for (int lsl_1 = 0;lsl_1 < 32;lsl_1++) {
                    int x = 1;
                    int y;
                    y = (x - (x << lsl_1));
                    if (y == v) {
                        flag = true;
                        vec.push_back(new GRegRegInstr(GRegRegInstr::Sub, getGR(mulIir->res.getVal()),
                                                       getGR(mulIir->left.getVal()),
                                                       getGR(mulIir->left.getVal()), GRegRegInstr::LSL,lsl_1));
                        break;
                    }
                }
            }
            //rsb inst*1
            {
                for (int lsl_1 = 0;lsl_1 < 32;lsl_1++) {
                    int x = 1;
                    int y;
                    y = ((x << lsl_1) - x);
                    if (y == v) {
                        flag = true;
                        vec.push_back(new GRegRegInstr(GRegRegInstr::RSUB, getGR(mulIir->res.getVal()),
                                                       getGR(mulIir->left.getVal()),
                                                       getGR(mulIir->left.getVal()), GRegRegInstr::LSL,lsl_1));
                        break;
                    }
                }
            }
            //lsl inst*1
            {
                for (int lsl_1 = 0;lsl_1 < 32;lsl_1++) {
                    int x = 1;
                    int y;
                    y = (x << lsl_1);
                    if (y == v) {
                        flag = true;
                        vec.push_back(new LSImmInstr(LSImmInstr::LSL, getGR(mulIir->res.getVal()),
                                                     getGR(mulIir->left.getVal()),lsl_1));
                        break;
                    }
                }
            }
            if (!flag) {
                getConstantGR(mulIir->right.getInt(),block,vec);
                vec.push_back(new GRegRegInstr(GRegRegInstr::Mul,
                                               getGR(mulIir->res.getVal()),
                                               getGR(mulIir->left.getVal()),
                                               constantIntMapping[block][mulIir->right.getInt()]));
            }
        } else {
            vec.push_back(new GRegRegInstr(GRegRegInstr::Mul, getGR(mulIir->res.getVal()), getGR(mulIir->left.getVal()),
                                           getGR(mulIir->right.getVal())));
        }
        return vec;
    }
    if (typeid(*ir) == typeid(DivIIR)) {
        std::vector<Instr*> vec;
        DivIIR* divIir = dynamic_cast<DivIIR*>(ir);
        if (!divIir->right.getVal()) {
            int v = divIir->right.getInt();
            int i = 0;
            for (i = 0; i < 32; ++i) {
                int x = 1;
                int y = x << i;
                if (y == v) {
                    vec.push_back(new MoveReg(getGR(divIir->res.getVal()),
                                              getGR(divIir->left.getVal())));
                    vec.push_back(new CmpImm(getGR(divIir->res.getVal()),0));
                    vec.push_back(new GRegImmInstr(GRegImmInstr::Add,
                                                   getGR(divIir->res.getVal()),
                                                   getGR(divIir->res.getVal()),
                                                   1,COND::LT));
                    vec.push_back(new MoveReg(getGR(divIir->res.getVal()), getGR(divIir->res.getVal()), i));
                    break;
                }
            }
            if (i == 32) {
                getConstantGR(divIir->right.getInt(),block,vec);
                vec.push_back(new GRegRegInstr(GRegRegInstr::Div,
                                               getGR(divIir->res.getVal()),
                                               getGR(divIir->left.getVal()),
                                               constantIntMapping[block][divIir->right.getInt()]));
            }
        } else if (!divIir->left.getVal()){
            if (!divIir->left.getVal()) {
                getConstantGR(divIir->left.getInt(),block,vec);
                vec.push_back(new GRegRegInstr(GRegRegInstr::Div,
                                               getGR(divIir->res.getVal()),
                                               constantIntMapping[block][divIir->left.getInt()],
                                               getGR(divIir->right.getVal())));
            }
        } else {
            vec.push_back(new GRegRegInstr(GRegRegInstr::Div, getGR(divIir->res.getVal()), getGR(divIir->left.getVal()),
                                           getGR(divIir->right.getVal())));
        }
        return vec;
    }
    if (typeid(*ir) == typeid(ModIR)) {
        ModIR* modIr = dynamic_cast<ModIR*>(ir);
        std::vector<Instr*> vec;
        GR res_gr = getGR(modIr->res.getVal());
        GR left_gr,right_gr;
        if (modIr->left.getVal()) {
            left_gr = getGR(modIr->left.getVal());
        } else {
            left_gr = getConstantGR(modIr->left.getInt(),block,vec);
        }
        if (modIr->right.getVal()) {
            right_gr = getGR(modIr->right.getVal());
        } else {
            right_gr = getConstantGR(modIr->right.getInt(),block,vec);
        }
        vec.push_back(new GRegRegInstr(GRegRegInstr::Div,res_gr,left_gr,right_gr));
        vec.push_back(new GRegRegInstr(GRegRegInstr::Mul,res_gr,res_gr,right_gr));
        vec.push_back(new GRegRegInstr(GRegRegInstr::Sub,res_gr,left_gr,res_gr));
        return vec;
    }
    if (typeid(*ir) == typeid(LTIIR)) {
        LTIIR* ir2 = dynamic_cast<LTIIR*>(ir);
        std::vector<Instr*> vec;
        if (!ir2->right.getVal()) {
            if (is_legal_immediate(ir2->right.getInt())) {
                vec.push_back(new CmpImm(getGR(ir2->left.getVal()), ir2->right.getInt()));
            } else {
                GR right_gr = getConstantGR(ir2->right.getInt(),block,vec);
                vec.push_back(new Cmp(getGR(ir2->left.getVal()),right_gr));
            }
            vec.push_back(new MovImm(getGR(ir2->res.getVal()), 0));
            vec.push_back(new MovImm(getGR(ir2->res.getVal()), 1, LT));
        } else if (!ir2->left.getVal()) {
            if (is_legal_immediate(ir2->left.getInt())) {
                vec.push_back(new CmpImm(getGR(ir2->right.getVal()), ir2->left.getInt()));
            } else {
                GR left_gr = getConstantGR(ir2->left.getInt(),block,vec);
                vec.push_back(new Cmp(getGR(ir2->right.getVal()),left_gr));
            }
            vec.push_back(new MovImm(getGR(ir2->res.getVal()), 0));
            vec.push_back(new MovImm(getGR(ir2->res.getVal()), 1, GE));
        } else {
            vec.push_back(new Cmp(getGR(ir2->left.getVal()), getGR(ir2->right.getVal())));
            vec.push_back(new MovImm(getGR(ir2->res.getVal()), 0));
            vec.push_back(new MovImm(getGR(ir2->res.getVal()), 1, LT));
        }
        return vec;
    }
    if (typeid(*ir) == typeid(LEIIR)) {
        LEIIR* ir2 = dynamic_cast<LEIIR*>(ir);
        std::vector<Instr*> vec;
        if (!ir2->right.getVal()) {
            if (is_legal_immediate(ir2->right.getInt())) {
                vec.push_back(new CmpImm(getGR(ir2->left.getVal()), ir2->right.getInt()));
            } else {
                GR right_gr = getConstantGR(ir2->right.getInt(),block,vec);
                vec.push_back(new Cmp(getGR(ir2->left.getVal()),right_gr));
            }
            vec.push_back(new MovImm(getGR(ir2->res.getVal()), 0));
            vec.push_back(new MovImm(getGR(ir2->res.getVal()), 1, LE));
        } else if (!ir2->left.getVal()) {
            if (is_legal_immediate(ir2->left.getInt())) {
                vec.push_back(new CmpImm(getGR(ir2->right.getVal()), ir2->left.getInt()));
            } else {
                GR left_gr = getConstantGR(ir2->left.getInt(),block,vec);
                vec.push_back(new Cmp(getGR(ir2->right.getVal()),left_gr));
            }
            vec.push_back(new MovImm(getGR(ir2->res.getVal()), 0));
            vec.push_back(new MovImm(getGR(ir2->res.getVal()), 1, GT));
        } else {
            vec.push_back(new Cmp(getGR(ir2->left.getVal()), getGR(ir2->right.getVal())));
            vec.push_back(new MovImm(getGR(ir2->res.getVal()), 0));
            vec.push_back(new MovImm(getGR(ir2->res.getVal()), 1, LE));
        }
        return vec;
    }
    if (typeid(*ir) == typeid(GTIIR)) {
        GTIIR* ir2 = dynamic_cast<GTIIR*>(ir);
        std::vector<Instr*> vec;
        if (!ir2->right.getVal()) {
            if (is_legal_immediate(ir2->right.getInt())) {
                vec.push_back(new CmpImm(getGR(ir2->left.getVal()), ir2->right.getInt()));
            } else {
                GR right_gr = getConstantGR(ir2->right.getInt(),block,vec);
                vec.push_back(new Cmp(getGR(ir2->left.getVal()),right_gr));
            }
            vec.push_back(new MovImm(getGR(ir2->res.getVal()), 0));
            vec.push_back(new MovImm(getGR(ir2->res.getVal()), 1, GT));
        } else if (!ir2->left.getVal()) {
            if (is_legal_immediate(ir2->left.getInt())) {
                vec.push_back(new CmpImm(getGR(ir2->right.getVal()), ir2->left.getInt()));
            } else {
                GR left_gr = getConstantGR(ir2->left.getInt(),block,vec);
                vec.push_back(new Cmp(getGR(ir2->right.getVal()),left_gr));
            }
            vec.push_back(new MovImm(getGR(ir2->res.getVal()), 0));
            vec.push_back(new MovImm(getGR(ir2->res.getVal()), 1, LE));
        } else {
            vec.push_back(new Cmp(getGR(ir2->left.getVal()), getGR(ir2->right.getVal())));
            vec.push_back(new MovImm(getGR(ir2->res.getVal()), 0));
            vec.push_back(new MovImm(getGR(ir2->res.getVal()), 1, GT));
        }
        return vec;
    }
    if (typeid(*ir) == typeid(GEIIR)) {
        GEIIR* ir2 = dynamic_cast<GEIIR*>(ir);
        std::vector<Instr*> vec;
        if (!ir2->right.getVal()) {
            if (is_legal_immediate(ir2->right.getInt())) {
                vec.push_back(new CmpImm(getGR(ir2->left.getVal()), ir2->right.getInt()));
            } else {
                GR right_gr = getConstantGR(ir2->right.getInt(),block,vec);
                vec.push_back(new Cmp(getGR(ir2->left.getVal()),right_gr));
            }
            vec.push_back(new MovImm(getGR(ir2->res.getVal()), 0));
            vec.push_back(new MovImm(getGR(ir2->res.getVal()), 1, GE));
        } else if (!ir2->left.getVal()) {
            if (is_legal_immediate(ir2->left.getInt())) {
                vec.push_back(new CmpImm(getGR(ir2->right.getVal()), ir2->left.getInt()));
            } else {
                GR left_gr = getConstantGR(ir2->left.getInt(),block,vec);
                vec.push_back(new Cmp(getGR(ir2->right.getVal()),left_gr));
            }
            vec.push_back(new MovImm(getGR(ir2->res.getVal()), 0));
            vec.push_back(new MovImm(getGR(ir2->res.getVal()), 1, LT));
        } else {
            vec.push_back(new Cmp(getGR(ir2->left.getVal()), getGR(ir2->right.getVal())));
            vec.push_back(new MovImm(getGR(ir2->res.getVal()), 0));
            vec.push_back(new MovImm(getGR(ir2->res.getVal()), 1, GE));
        }
        return vec;
    }
    if (typeid(*ir) == typeid(EQUIIR)) {
        EQUIIR* ir2 = dynamic_cast<EQUIIR*>(ir);
        std::vector<Instr*> vec;
        if (!ir2->right.getVal()) {
            if (is_legal_immediate(ir2->right.getInt())) {
                vec.push_back(new CmpImm(getGR(ir2->left.getVal()), ir2->right.getInt()));
            } else {
                GR right_gr = getConstantGR(ir2->right.getInt(),block,vec);
                vec.push_back(new Cmp(getGR(ir2->left.getVal()),right_gr));
            }
            vec.push_back(new MovImm(getGR(ir2->res.getVal()), 0));
            vec.push_back(new MovImm(getGR(ir2->res.getVal()), 1, EQU));
        } else if (!ir2->left.getVal()) {
            if (is_legal_immediate(ir2->left.getInt())) {
                vec.push_back(new CmpImm(getGR(ir2->right.getVal()), ir2->left.getInt()));
            } else {
                GR left_gr = getConstantGR(ir2->left.getInt(),block,vec);
                vec.push_back(new Cmp(getGR(ir2->right.getVal()),left_gr));
            }
            vec.push_back(new MovImm(getGR(ir2->res.getVal()), 0));
            vec.push_back(new MovImm(getGR(ir2->res.getVal()), 1, NE));
        } else {
            vec.push_back(new Cmp(getGR(ir2->left.getVal()), getGR(ir2->right.getVal())));
            vec.push_back(new MovImm(getGR(ir2->res.getVal()), 0));
            vec.push_back(new MovImm(getGR(ir2->res.getVal()), 1, EQU));
        }
        return vec;
    }
    if (typeid(*ir) == typeid(NEIIR)) {
        NEIIR* ir2 = dynamic_cast<NEIIR*>(ir);
        std::vector<Instr*> vec;
        if (!ir2->right.getVal()) {
            if (is_legal_immediate(ir2->right.getInt())) {
                vec.push_back(new CmpImm(getGR(ir2->left.getVal()), ir2->right.getInt()));
            } else {
                GR right_gr = getConstantGR(ir2->right.getInt(),block,vec);
                vec.push_back(new Cmp(getGR(ir2->left.getVal()),right_gr));
            }
            vec.push_back(new MovImm(getGR(ir2->res.getVal()), 0));
            vec.push_back(new MovImm(getGR(ir2->res.getVal()), 1, NE));
        } else if (!ir2->left.getVal()) {
            if (is_legal_immediate(ir2->left.getInt())) {
                vec.push_back(new CmpImm(getGR(ir2->right.getVal()), ir2->left.getInt()));
            } else {
                GR left_gr = getConstantGR(ir2->left.getInt(),block,vec);
                vec.push_back(new Cmp(getGR(ir2->right.getVal()),left_gr));
            }
            vec.push_back(new MovImm(getGR(ir2->res.getVal()), 0));
            vec.push_back(new MovImm(getGR(ir2->res.getVal()), 1, EQU));
        } else {
            vec.push_back(new Cmp(getGR(ir2->left.getVal()), getGR(ir2->right.getVal())));
            vec.push_back(new MovImm(getGR(ir2->res.getVal()), 0));
            vec.push_back(new MovImm(getGR(ir2->res.getVal()), 1, NE));
        }
        return vec;
    }

    if (typeid(*ir) == typeid(AddFIR) || typeid(*ir) == typeid(SubFIR) ||
        typeid(*ir) == typeid(MulFIR) || typeid(*ir) == typeid(DivFIR) ||
        typeid(*ir) == typeid(LTFIR) || typeid(*ir) == typeid(GTFIR) ||
        typeid(*ir) == typeid(LEFIR) || typeid(*ir) == typeid(GEFIR) ||
        typeid(*ir) == typeid(EQUFIR) || typeid(*ir) == typeid(NEFIR)) {
        ArithmeticIR *ir2 = dynamic_cast<ArithmeticIR *>(ir);
        std::vector<Instr *> vec;
        if (!ir2->left.getVal()) {
            Value *v = new VarValue();
            ir2->left.setVal(v);
            if (ir2->left.isInt()) {
                GR dst = getGR(ir2->left.getVal());
                std::vector<Instr *> v = setIntValue(dst, ir2->left.getInt());
                for (Instr *instr: v) {
                    vec.push_back(instr);
                }
            } else {
                FR dst = getFR(ir2->left.getVal());
                vec.push_back(new MoveWFromSymbol(GR(12), getFloatAddr(ir2->left.getFloat())));
                vec.push_back(new MoveTFromSymbol(GR(12), getFloatAddr(ir2->left.getFloat())));
                vec.push_back(new VLoad(dst, GR(12),0));
            }
        }
        if (!ir2->right.getVal()) {
            Value *v = new VarValue();
            ir2->right.setVal(v);
            if (ir2->right.isInt()) {
                GR dst = getGR(ir2->right.getVal());
                std::vector<Instr *> v = setIntValue(dst, ir2->right.getInt());
                for (Instr *instr: v) {
                    vec.push_back(instr);
                }
            } else {
                FR dst = getFR(ir2->right.getVal());
                vec.push_back(new MoveWFromSymbol(GR(12), getFloatAddr(ir2->right.getFloat())));
                vec.push_back(new MoveTFromSymbol(GR(12), getFloatAddr(ir2->right.getFloat())));
                vec.push_back(new VLoad(dst, GR(12),0));
            }
        }
        if (typeid(*ir) == typeid(AddFIR)) {
            vec.push_back(new VRegRegInstr(VRegRegInstr::VAdd,
                                           getFR(ir2->res.getVal()),
                                           getFR(ir2->left.getVal()),
                                           getFR(ir2->right.getVal())));
        }
        if (typeid(*ir) == typeid(SubFIR)) {
            vec.push_back(new VRegRegInstr(VRegRegInstr::VSub,
                                           getFR(ir2->res.getVal()),
                                           getFR(ir2->left.getVal()),
                                           getFR(ir2->right.getVal())));
        }
        if (typeid(*ir) == typeid(MulFIR)) {
            vec.push_back(new VRegRegInstr(VRegRegInstr::VMul,
                                           getFR(ir2->res.getVal()),
                                           getFR(ir2->left.getVal()),
                                           getFR(ir2->right.getVal())));
        }
        if (typeid(*ir) == typeid(DivFIR)) {
            vec.push_back(new VRegRegInstr(VRegRegInstr::VDiv,
                                           getFR(ir2->res.getVal()),
                                           getFR(ir2->left.getVal()),
                                           getFR(ir2->right.getVal())));
        }
        if (typeid(*ir) == typeid(LTFIR)) {
            vec.push_back(new VCmpe(getFR(ir2->left.getVal()), getFR(ir2->right.getVal())));
            vec.push_back(new VMrs());
            vec.push_back(new MovImm(getGR(ir2->res.getVal()), 0));
            vec.push_back(new MovImm(getGR(ir2->res.getVal()), 1, LT));
        }
        if (typeid(*ir) == typeid(GTFIR)) {
            vec.push_back(new VCmpe(getFR(ir2->left.getVal()), getFR(ir2->right.getVal())));
            vec.push_back(new VMrs());
            vec.push_back(new MovImm(getGR(ir2->res.getVal()), 0));
            vec.push_back(new MovImm(getGR(ir2->res.getVal()), 1, GT));
        }
        if (typeid(*ir) == typeid(LEFIR)) {
            vec.push_back(new VCmpe(getFR(ir2->left.getVal()), getFR(ir2->right.getVal())));
            vec.push_back(new VMrs());
            vec.push_back(new MovImm(getGR(ir2->res.getVal()), 0));
            vec.push_back(new MovImm(getGR(ir2->res.getVal()), 1, LE));
        }
        if (typeid(*ir) == typeid(GEFIR)) {
            vec.push_back(new VCmpe(getFR(ir2->left.getVal()), getFR(ir2->right.getVal())));
            vec.push_back(new VMrs());
            vec.push_back(new MovImm(getGR(ir2->res.getVal()), 0));
            vec.push_back(new MovImm(getGR(ir2->res.getVal()), 1, GE));
        }
        if (typeid(*ir) == typeid(EQUFIR)) {
            vec.push_back(new VCmpe(getFR(ir2->left.getVal()), getFR(ir2->right.getVal())));
            vec.push_back(new VMrs());
            vec.push_back(new MovImm(getGR(ir2->res.getVal()), 0));
            vec.push_back(new MovImm(getGR(ir2->res.getVal()), 1, EQU));
        }
        if (typeid(*ir) == typeid(NEFIR)) {
            vec.push_back(new VCmpe(getFR(ir2->left.getVal()), getFR(ir2->right.getVal())));
            vec.push_back(new VMrs());
            vec.push_back(new MovImm(getGR(ir2->res.getVal()), 0));
            vec.push_back(new MovImm(getGR(ir2->res.getVal()), 1, NE));
        }
        return vec;
    }
    if (typeid(*ir) == typeid(JumpIR)) {
        return {new B(dynamic_cast<JumpIR *>(ir)->target->name)};
    }
    if (typeid(*ir) == typeid(BranchIR)) {
        BranchIR *branchIr = dynamic_cast<BranchIR *>(ir);
        return {new CmpImm(getGR(branchIr->cond), 0),
                new B(branchIr->trueTarget->name, NE),
                new B(branchIr->falseTarget->name, EQU)};
    }
    if (typeid(*ir) == typeid(CallIR)) {
        CallIR *callIr = dynamic_cast<CallIR *>(ir);
        int gr_cnt = 0;
        int fr_cnt = 0;
        int cnt = 0;
        std::vector<Instr *> vec;
        for (TempVal v: callIr->args) {
            std::vector<Instr *> tempVec;
            if (v.getType()->isString()) {
                tempVec.push_back(new MoveWFromSymbol(GR(0), stringConstMapping[v.getString()]));
                tempVec.push_back(new MoveTFromSymbol(GR(0), stringConstMapping[v.getString()]));
                gr_cnt++;
                continue;
            }
            if (!v.isFloat()) {
                if (gr_cnt >= 4) {
                    if (!v.getVal()) {
                        v.setVal(new VarValue());
                        std::vector<Instr *> vv = setIntValue(getGR(v.getVal()), v.getInt());
                        for (Instr *instr: vv) {
                            tempVec.push_back(instr);
                        }
                    } else if (gRegMapping.count(v.getVal()) == 0) {
                        if (is_legal_load_store_offset(stackMapping[v.getVal()]) &&
                            is_legal_immediate(stackMapping[v.getVal()])) {
                            tempVec.push_back(new GRegImmInstr(GRegImmInstr::Add, getGR(v.getVal()), GR(13),
                                                               stackMapping[v.getVal()]));
                        } else {
                            std::vector<Instr *> vv = setIntValue(GR(12), stackMapping[v.getVal()]);
                            for (Instr *instr: vv) {
                                tempVec.push_back(instr);
                            }
                            tempVec.push_back(new GRegRegInstr(GRegRegInstr::Add, getGR(v.getVal()), GR(13), GR(12)));
                        }
                    }
                    stackMapping[v.getVal()] = cnt * 4;
                    cnt++;
                    tempVec.push_back(new Store(getGR(v.getVal()), GR(13), stackMapping[v.getVal()]));
                } else {
                    if (!v.getVal()) {
                        v.setVal(new VarValue());
                        gRegMapping[v.getVal()] = gr_cnt;
                        if (is_legal_load_store_offset(v.getInt()) && is_legal_immediate(v.getInt())) {
                            tempVec.push_back(new MovImm(getGR(v.getVal()), v.getInt()));
                        } else {
                            std::vector<Instr *> vv = setIntValue(getGR(v.getVal()), v.getInt());
                            for (Instr *instr: vv) {
                                tempVec.push_back(instr);
                            }
                        }
                    } else {
                        if (gRegMapping.count(v.getVal()) != 0) {
                            tempVec.push_back(new MoveReg(GR(gr_cnt), getGR(v.getVal())));
                            gRegMapping[v.getVal()] = gr_cnt;
                        } else {
                            if (is_legal_load_store_offset(stackMapping[v.getVal()]) &&
                                is_legal_immediate(stackMapping[v.getVal()])) {
                                tempVec.push_back(new GRegImmInstr(GRegImmInstr::Add, GR(gr_cnt), GR(13),
                                                                   stackMapping[v.getVal()]));
                            } else {
                                std::vector<Instr *> vv = setIntValue(GR(12), stackMapping[v.getVal()]);
                                for (Instr *instr: vv) {
                                    tempVec.push_back(instr);
                                }
                                tempVec.push_back(new GRegRegInstr(GRegRegInstr::Add, GR(gr_cnt), GR(13), GR(12)));
                            }
                        }
                    }
                }
                gr_cnt++;
            } else {
                if (fr_cnt >= 32) {
                    if (!v.getVal()) {
                        v.setVal(new VarValue());
                        tempVec.push_back(new MoveWFromSymbol(GR(12), getFloatAddr(v.getFloat())));
                        tempVec.push_back(new MoveTFromSymbol(GR(12), getFloatAddr(v.getFloat())));
                        tempVec.push_back(new VLoad(getFR(v.getVal()), GR(12), 0));
                    }
                    stackMapping[v.getVal()] = cnt * 4;
                    cnt++;
                    tempVec.push_back(new VStore(getFR(v.getVal()), GR(13), stackMapping[v.getVal()]));
                } else {
                    if (!v.getVal()) {
                        v.setVal(new VarValue());
                        fRegMapping[v.getVal()] = fr_cnt;
                        tempVec.push_back(new MoveWFromSymbol(GR(12), getFloatAddr(v.getFloat())));
                        tempVec.push_back(new MoveTFromSymbol(GR(12), getFloatAddr(v.getFloat())));
                        tempVec.push_back(new VLoad(getFR(v.getVal()), GR(12), 0));
                    } else {
                        tempVec.push_back(new VMoveReg(FR(fr_cnt), getFR(v.getVal())));
                        fRegMapping[v.getVal()] = fr_cnt;
                    }
                }
                fr_cnt++;
            }
            for (auto it = tempVec.rbegin(); it != tempVec.rend(); it++) {
                vec.insert(vec.begin(), *it);
            }
        }

//        vec.push_back(new Push({}));
//        vec.push_back(new MoveReg(GR(13),GR(11)));
        vec.push_back(new Bl(callIr->func->name));
//        vec.push_back(new MoveReg(GR(11),GR(13)));
//        vec.push_back(new Pop({}));
        if (callIr->returnVal) {
            if (callIr->returnVal->getType()->isInt()) {
                vec.push_back(new MoveReg(getGR(callIr->returnVal), GR(0)));
            } else {
                vec.push_back(new VMoveReg(getFR(callIr->returnVal), FR(0)));
            }
        }
        return vec;
    }
    if (typeid(*ir) == typeid(ReturnIR)) {
        ReturnIR *returnIr = dynamic_cast<ReturnIR *>(ir);
        std::vector<Instr *> vec;
        if (returnIr->useInt) {
            vec.push_back(new MovImm(GR(0), returnIr->retInt));
        } else if (returnIr->useFloat) {
            vec.push_back(new MoveWFromSymbol(GR(12), getFloatAddr(returnIr->retFloat)));
            vec.push_back(new MoveTFromSymbol(GR(12), getFloatAddr(returnIr->retFloat)));
            vec.push_back(new VLoad(FR(0), GR(12), 0));
        } else if (returnIr->v) {
            if (returnIr->v->getType()->isInt()) {
                vec.push_back(new MoveReg(GR(0), getGR(returnIr->v)));
            } else {
                vec.push_back(new VMoveReg(FR(0), getFR(returnIr->v)));
            }
        }
        vec.push_back(new Ret());
        return vec;
    }
    return {};
}

GR Codegen::getGR(Value *src) {
    if (gRegMapping.count(src) == 0) {
        GR reg = GR::allocateReg();
        gRegMapping[src] = reg;
        return reg;
    }
    return gRegMapping[src];
}

FR Codegen::getFR(Value *src) {
    if (fRegMapping.count(src) == 0) {
        FR reg = FR::allocateReg();
        fRegMapping[src] = reg;
        return reg;
    }
    return fRegMapping[src];
}

void Codegen::generateGlobalCode() {
    std::vector<Value *> dataList;
    std::vector<Value *> bssList;
    for (Value *v: irVisitor.globalVars) {
        if (typeid(*v) == typeid(ConstValue)) {
            dataList.push_back(v);
        } else {
            if (v->getType()->isString()) {
                dataList.push_back(v);
            } else {
                bssList.push_back(v);
            }
        }
    }
    if (!dataList.empty()) {
        out << ".section .data\n";
        for (Value *v: dataList) {
            if (!v->getType()->isString()) {
                ConstValue *vv = dynamic_cast<ConstValue *>(v);
                if (!vv->is_Array() && !vv->getType()->isInt()) {
                    if (floatConstMapping.count(vv->getFloatVal()) == 0) {
                        floatConstMapping[vv->getFloatVal()] = vv->getName();
                    }
                    continue;
                }
                out << ".align\n";
                out << v->getName() << ":\n";
                out << "\t.4byte ";
                if (!vv->is_Array()) {
                    if (vv->getType()->isInt()) {
                        out << vv->getIntVal();
                    }
                } else {
                    for (int i = 0; i < vv->getArrayLen(); ++i) {
                        if (i != 0) {
                            out << ",";
                        }
                        if (vv->getType()->isIntPointer()) {
                            out << vv->getIntValList()[i];
                        } else {
                            out << floatToInt(vv->getFloatValList()[i]);
                        }
                    }
                }
                out << "\n";
            } else {
                std::string varName = "_m_global_string_const" + std::to_string(stringConstCnt++);
                stringConstMapping[v->getName()] = varName;
                out << varName << ":\n\t.asciz " << v->getName() << "\n";
            }
        }
    }
    if (!bssList.empty()) {
        out << ".section .bss\n";
        for (Value *v: bssList) {
            out << ".align\n";
            out << v->getName() << ":\n";
            out << "\t.space ";
            if (!v->is_Array()) {
                out << "4";
            } else {
                out << v->getArrayLen() * 4;
            }
            out << "\n";
        }
    }
}

void Codegen::comment(std::string s) {
    out << "@ " << s << "\n";
}