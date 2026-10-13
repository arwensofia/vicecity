// SPDX-License-Identifier: GPL-3.0-or-later
// Recompiler semantic tests. Each snippet is assembled, translated by the real
// emitter (test_gen) and executed (test_runner). Expected values follow the
// EE Core Instruction Set Manual; corner cases follow PCSX2's interpreter.
//
// TESTCASE(name, ASM(insns...), body)   -- body runs with ps2_cpu *c; RUN() executes.
// TESTCASE_JT(name, table_addr, OFFS(word offsets), ASM(...), body)
//   -- also writes a jump table (start + 4*offset entries) at table_addr.
// The snippet is followed by "jr ra; nop". A local callee "jr ra; addiu v0, v0, 5"
// sits at start + 0x200 (reachable with REGIMM call offset 0x7F - index).

// ======================= ALU =======================
TESTCASE(addu_sext, ASM(addu(V0, A0, A1), addu(V1, A0, ZERO), add(T0, A0, A1)),
	GPR(A0) = 0xABCD00007FFFFFFFull; GPR(A1) = 1; GPRHI(V0) = 0x1111;
	RUN();
	CHECK_EQ(GPR(V0), 0xFFFFFFFF80000000ull);
	CHECK_EQ(GPRHI(V0), 0x1111); // upper 64 bits untouched
	CHECK_EQ(GPR(V1), 0x7FFFFFFFull);
	CHECK_EQ(GPR(T0), 0xFFFFFFFF80000000ull);)

TESTCASE(addiu_addi, ASM(addiu(V0, A0, -1), addiu(V1, ZERO, -32768), addiu(T0, A1, 1), addi(T1, A2, 100)),
	GPR(A0) = 0; GPR(A1) = 0x7FFFFFFF; GPR(A2) = 5;
	RUN();
	CHECK_EQ(GPR(V0), ~0ull);
	CHECK_EQ(GPR(V1), 0xFFFFFFFFFFFF8000ull);
	CHECK_EQ(GPR(T0), 0xFFFFFFFF80000000ull);
	CHECK_EQ(GPR(T1), 105);)

TESTCASE(sub_subu, ASM(subu(V0, A0, A1), sub(V1, A1, A0), sub(T0, A2, A3)),
	GPR(A0) = 0; GPR(A1) = 1; GPR(A2) = 5; GPR(A3) = static_cast<uint64_t>(-7ll);
	RUN();
	CHECK_EQ(GPR(V0), ~0ull);
	CHECK_EQ(GPR(V1), 1);
	CHECK_EQ(GPR(T0), 12);)

TESTCASE(dadd_dsub, ASM(daddu(V0, A0, A1), dadd(V1, A0, A1), dsubu(T0, A0, A1), dsub(T1, A1, A0),
                        daddi(T2, A0, -1), daddiu(T3, A1, 0x7FFF)),
	GPR(A0) = ~0ull; GPR(A1) = 2;
	RUN();
	CHECK_EQ(GPR(V0), 1);
	CHECK_EQ(GPR(V1), 1);
	CHECK_EQ(GPR(T0), 0xFFFFFFFFFFFFFFFDull);
	CHECK_EQ(GPR(T1), 3);
	CHECK_EQ(GPR(T2), 0xFFFFFFFFFFFFFFFEull);
	CHECK_EQ(GPR(T3), 0x8001);)

TESTCASE(logic, ASM(and_(V0, A0, A1), or_(V1, A0, A1), xor_(T0, A0, A1), nor(T1, A0, A1),
                    andi(T2, A0, 0xFFFF), ori(T3, A0, 0x00FF), xori(T4, A0, 0xFFFF), or_(T5, A0, ZERO)),
	GPR(A0) = 0xFF00FF00FF00FF00ull; GPR(A1) = 0x0FF00FF00FF00FF0ull;
	RUN();
	CHECK_EQ(GPR(V0), 0x0F000F000F000F00ull);
	CHECK_EQ(GPR(V1), 0xFFF0FFF0FFF0FFF0ull);
	CHECK_EQ(GPR(T0), 0xF0F0F0F0F0F0F0F0ull);
	CHECK_EQ(GPR(T1), 0x000F000F000F000Full);
	CHECK_EQ(GPR(T2), 0xFF00);
	CHECK_EQ(GPR(T3), 0xFF00FF00FF00FFFFull);
	CHECK_EQ(GPR(T4), 0xFF00FF00FF0000FFull);
	CHECK_EQ(GPR(T5), 0xFF00FF00FF00FF00ull);)

TESTCASE(lui_li, ASM(lui(V0, 0x8000), lui(V1, 0x1234), ori(V1, V1, 0x5678)),
	RUN();
	CHECK_EQ(GPR(V0), 0xFFFFFFFF80000000ull);
	CHECK_EQ(GPR(V1), 0x12345678);)

TESTCASE(slt_family, ASM(slt(V0, A0, A1), sltu(V1, A0, A1), slti(T0, A0, 0), sltiu(T1, A1, -1),
                         sltiu(T2, A0, -1), slti(T3, A1, 1), slt(T4, A3, A2)),
	GPR(A0) = ~0ull; GPR(A1) = 1; GPR(A2) = 0x100000000ull; GPR(A3) = 1;
	RUN();
	CHECK_EQ(GPR(V0), 1);
	CHECK_EQ(GPR(V1), 0);
	CHECK_EQ(GPR(T0), 1);
	CHECK_EQ(GPR(T1), 1); // sltiu compares against the sign-extended immediate
	CHECK_EQ(GPR(T2), 0);
	CHECK_EQ(GPR(T3), 0);
	CHECK_EQ(GPR(T4), 1);) // 64-bit compare

TESTCASE(movz_movn, ASM(movz(V0, A0, ZERO), movz(V1, A0, A1), movn(T0, A0, A1), movn(T1, A0, ZERO), movz(T2, A0, A2)),
	GPR(A0) = 0x1234567890ull; GPR(A1) = 1; GPR(A2) = 0x100000000ull; GPR(V1) = 7; GPR(T1) = 9; GPR(T2) = 3;
	RUN();
	CHECK_EQ(GPR(V0), 0x1234567890ull);
	CHECK_EQ(GPR(V1), 7);
	CHECK_EQ(GPR(T0), 0x1234567890ull);
	CHECK_EQ(GPR(T1), 9);
	CHECK_EQ(GPR(T2), 3);) // rt tested as 64 bits

// ======================= shifts =======================
TESTCASE(shift32, ASM(sll(V0, A0, 1), sll(V1, A0, 0), srl(T0, A0, 4), srl(T1, A0, 0), sra(T2, A0, 4),
                      sllv(T3, A0, A1), srlv(T4, A0, A1), srav(T5, A0, A1)),
	GPR(A0) = 0x0000000080000001ull; GPR(A1) = 33;
	RUN();
	CHECK_EQ(GPR(V0), 2);
	CHECK_EQ(GPR(V1), 0xFFFFFFFF80000001ull);
	CHECK_EQ(GPR(T0), 0x08000000);
	CHECK_EQ(GPR(T1), 0xFFFFFFFF80000001ull);
	CHECK_EQ(GPR(T2), 0xFFFFFFFFF8000000ull);
	CHECK_EQ(GPR(T3), 2);
	CHECK_EQ(GPR(T4), 0x40000000);
	CHECK_EQ(GPR(T5), 0xFFFFFFFFC0000000ull);)

TESTCASE(shift64, ASM(dsll(V0, A0, 4), dsrl(V1, A0, 4), dsra(T0, A0, 4), dsll32(T1, A0, 0), dsrl32(T2, A0, 4),
                      dsra32(T3, A0, 0), dsllv(T4, A0, A1), dsrlv(T5, A0, A1), dsrav(T6, A0, A1)),
	GPR(A0) = 0x8000000000000001ull; GPR(A1) = 65;
	RUN();
	CHECK_EQ(GPR(V0), 0x10);
	CHECK_EQ(GPR(V1), 0x0800000000000000ull);
	CHECK_EQ(GPR(T0), 0xF800000000000000ull);
	CHECK_EQ(GPR(T1), 0x0000000100000000ull);
	CHECK_EQ(GPR(T2), 0x08000000);
	CHECK_EQ(GPR(T3), 0xFFFFFFFF80000000ull);
	CHECK_EQ(GPR(T4), 2);
	CHECK_EQ(GPR(T5), 0x4000000000000000ull);
	CHECK_EQ(GPR(T6), 0xC000000000000000ull);)

// ======================= HI/LO =======================
TESTCASE(mult_multu, ASM(mult(V0, A0, A1), mflo(V1), mfhi(T0), multu(T1, A0, A1), mfhi(T2)),
	GPR(A0) = static_cast<uint64_t>(-3ll); GPR(A1) = 0x40000000;
	RUN();
	CHECK_EQ(GPR(V0), 0x40000000); // 3-operand form writes rd = LO
	CHECK_EQ(GPR(V1), 0x40000000);
	CHECK_EQ(GPR(T0), ~0ull);
	CHECK_EQ(GPR(T1), 0x40000000);
	CHECK_EQ(GPR(T2), 0x3FFFFFFF);)

TESTCASE(div_signed, ASM(div_(A0, A1), mflo(V0), mfhi(V1), div_(A2, ZERO), mflo(T0), mfhi(T1),
                         div_(A3, T9), mflo(T2), mfhi(T3), div_(A0, ZERO), mflo(T4), mfhi(T5)),
	GPR(A0) = static_cast<uint64_t>(-7ll); GPR(A1) = 2; GPR(A2) = 5; GPR(A3) = 0xFFFFFFFF80000000ull; GPR(T9) = ~0ull;
	RUN();
	CHECK_EQ(GPR(V0), static_cast<uint64_t>(-3ll));
	CHECK_EQ(GPR(V1), ~0ull);
	CHECK_EQ(GPR(T0), ~0ull); // x/0, x >= 0: LO = -1, HI = x
	CHECK_EQ(GPR(T1), 5);
	CHECK_EQ(GPR(T2), 0xFFFFFFFF80000000ull); // INT_MIN / -1
	CHECK_EQ(GPR(T3), 0);
	CHECK_EQ(GPR(T4), 1); // x/0, x < 0: LO = 1
	CHECK_EQ(GPR(T5), static_cast<uint64_t>(-7ll));)

TESTCASE(divu, ASM(divu(A0, A1), mflo(V0), mfhi(V1), divu(A2, ZERO), mflo(T0), mfhi(T1), divu(T9, T8), mflo(T2)),
	GPR(A0) = 0xFFFFFFF9; GPR(A1) = 2; GPR(A2) = 5; GPR(T9) = 0xFFFFFFFF; GPR(T8) = 1;
	RUN();
	CHECK_EQ(GPR(V0), 0x7FFFFFFC);
	CHECK_EQ(GPR(V1), 1);
	CHECK_EQ(GPR(T0), ~0ull);
	CHECK_EQ(GPR(T1), 5);
	CHECK_EQ(GPR(T2), ~0ull);) // quotient sign-extended

TESTCASE(mthi_mtlo, ASM(mthi(A0), mtlo(A1), mfhi(V0), mflo(V1)),
	GPR(A0) = 0x1122334455667788ull; GPR(A1) = 0x99AABBCCDDEEFF00ull;
	RUN();
	CHECK_EQ(GPR(V0), 0x1122334455667788ull);
	CHECK_EQ(GPR(V1), 0x99AABBCCDDEEFF00ull);)

TESTCASE(pipeline1, ASM(mult1(V0, A0, A1), mflo1(V1), mfhi1(T0), div1(A0, A1), mflo1(T1), mfhi1(T2),
                        multu1(T3, A2, A2), mfhi1(T4), divu1(A2, A1), mflo1(T5), mfhi1(T6), mthi1(A0), mtlo1(A1)),
	GPR(A0) = 6; GPR(A1) = 7; GPR(A2) = 0xFFFFFFFF; c->lo.ud[0] = 0x55; c->hi.ud[0] = 0x66;
	RUN();
	CHECK_EQ(GPR(V0), 42);
	CHECK_EQ(GPR(V1), 42);
	CHECK_EQ(GPR(T0), 0);
	CHECK_EQ(GPR(T1), 0);
	CHECK_EQ(GPR(T2), 6);
	CHECK_EQ(GPR(T3), 1);
	CHECK_EQ(GPR(T4), 0xFFFFFFFFFFFFFFFEull);
	CHECK_EQ(GPR(T5), 0x24924924);
	CHECK_EQ(GPR(T6), 3);
	CHECK_EQ(c->hi.ud[1], 6);
	CHECK_EQ(c->lo.ud[1], 7);
	CHECK_EQ(c->lo.ud[0], 0x55); // pipeline 0 untouched
	CHECK_EQ(c->hi.ud[0], 0x66);)

TESTCASE(madd_family, ASM(madd(V0, A0, A1), maddu(V1, A2, A2), madd1(T0, A0, A1), maddu1(T1, A0, A3)),
	c->hi.ud[0] = 0; c->lo.ud[0] = 0xFFFFFFFF; c->hi.ud[1] = 0; c->lo.ud[1] = 10;
	GPR(A0) = 2; GPR(A1) = 3; GPR(A2) = 0xFFFFFFFF; GPR(A3) = ~0ull;
	RUN();
	CHECK_EQ(GPR(V0), 5);
	CHECK_EQ(GPR(V1), 6);
	CHECK_EQ(c->lo.ud[0], 6);
	CHECK_EQ(c->hi.ud[0], ~0ull);
	CHECK_EQ(GPR(T0), 16);
	CHECK_EQ(GPR(T1), 0xE);
	CHECK_EQ(c->lo.ud[1], 0xE);
	CHECK_EQ(c->hi.ud[1], 2);)

// ======================= SA register / QFSRV =======================
TESTCASE(sa_regs, ASM(mtsab(A0, 3), mfsa(V0), mtsah(A1, 1), mfsa(V1), mtsa(A2), mfsa(T0)),
	GPR(A0) = 5; GPR(A1) = 2; GPR(A2) = 0xB;
	RUN();
	CHECK_EQ(GPR(V0), 6);
	CHECK_EQ(GPR(V1), 6);
	CHECK_EQ(GPR(T0), 0xB);)

TESTCASE(qfsrv, ASM(mtsab(ZERO, 4), mmi1(27, V0, A0, A1), mtsab(ZERO, 8), mmi1(27, V1, A0, A1),
                    mtsab(ZERO, 12), mmi1(27, T0, A0, A1), mtsab(ZERO, 0), mmi1(27, T1, A0, A1)),
	set128(c->r[A0], 0x0123456789ABCDEFull, 0xFEDCBA9876543210ull);
	set128(c->r[A1], 0x0011223344556677ull, 0x8899AABBCCDDEEFFull);
	RUN();
	CHECK_EQ(GPR(V0), 0xCCDDEEFF00112233ull); CHECK_EQ(GPRHI(V0), 0x89ABCDEF8899AABBull);
	CHECK_EQ(GPR(V1), 0x8899AABBCCDDEEFFull); CHECK_EQ(GPRHI(V1), 0x0123456789ABCDEFull);
	CHECK_EQ(GPR(T0), 0x89ABCDEF8899AABBull); CHECK_EQ(GPRHI(T0), 0x7654321001234567ull);
	CHECK_EQ(GPR(T1), 0x0011223344556677ull); CHECK_EQ(GPRHI(T1), 0x8899AABBCCDDEEFFull);)

// ======================= loads / stores =======================
TESTCASE(loads, ASM(lb(V0, 1, A0), lbu(V1, 1, A0), lh(T0, 2, A0), lhu(T1, 2, A0), lw(T2, 4, A0), lwu(T3, 4, A0),
                    ld(T4, 8, A0), lw(T5, 8, A0), lw(T6, -16, A1), lw(ZERO, 0, A0)),
	w64(0x1000, 0x8786858483828180ull); w64(0x1008, 0x0F0E0D0C0B0A0908ull); GPR(A0) = 0x1000; GPR(A1) = 0x1010;
	RUN();
	CHECK_EQ(GPR(V0), 0xFFFFFFFFFFFFFF81ull);
	CHECK_EQ(GPR(V1), 0x81);
	CHECK_EQ(GPR(T0), 0xFFFFFFFFFFFF8382ull);
	CHECK_EQ(GPR(T1), 0x8382);
	CHECK_EQ(GPR(T2), 0xFFFFFFFF87868584ull);
	CHECK_EQ(GPR(T3), 0x87868584);
	CHECK_EQ(GPR(T4), 0x0F0E0D0C0B0A0908ull);
	CHECK_EQ(GPR(T5), 0x0B0A0908);
	CHECK_EQ(GPR(T6), 0xFFFFFFFF83828180ull);)

TESTCASE(lq_aligns, ASM(lq(V0, 5, A0), lq(ZERO, 0, A0)),
	w64(0x1000, 0x8786858483828180ull); w64(0x1008, 0x0F0E0D0C0B0A0908ull); GPR(A0) = 0x1000;
	RUN();
	CHECK_EQ(GPR(V0), 0x8786858483828180ull);
	CHECK_EQ(GPRHI(V0), 0x0F0E0D0C0B0A0908ull);)

TESTCASE(stores, ASM(sb(A1, 0, A0), sh(A1, 2, A0), sw(A1, 4, A0), sd(A1, 8, A0), sq(A2, 0x1F, A0)),
	GPR(A0) = 0x2000; GPR(A1) = 0x1122334455667788ull; set128(c->r[A2], 0xAAAABBBBCCCCDDDDull, 0x1111222233334444ull);
	RUN();
	CHECK_EQ(r64(0x2000), 0x5566778877880088ull);
	CHECK_EQ(r64(0x2008), 0x1122334455667788ull);
	CHECK_EQ(r64(0x2010), 0xAAAABBBBCCCCDDDDull);
	CHECK_EQ(r64(0x2018), 0x1111222233334444ull);)

TESTCASE(lwl_lwr, ASM(lwr(V0, 0, A0), lwl(V0, 3, A0), lwl(V1, 1, A2), lwr(T0, 2, A2), lwr(T1, 0, A2)),
	w64(0x3000, 0x7766554433221100ull); GPR(A0) = 0x3001; GPR(A2) = 0x3000;
	GPR(V1) = 0xFFFFFFFFAABBCCDDull; GPR(T0) = 0x12345678AABBCCDDull; GPR(T1) = 0x1234567800000000ull;
	RUN();
	CHECK_EQ(GPR(V0), 0x44332211);
	CHECK_EQ(GPR(V1), 0x1100CCDD);
	CHECK_EQ(GPR(T0), 0x12345678AABB3322ull); // LWR not ending a word: upper half kept
	CHECK_EQ(GPR(T1), 0x33221100);)

TESTCASE(ldl_ldr, ASM(ldr(V0, 0, A0), ldl(V0, 7, A0)),
	w64(0x3000, 0x7766554433221100ull); w64(0x3008, 0xFFEEDDCCBBAA9988ull); GPR(A0) = 0x3003;
	RUN();
	CHECK_EQ(GPR(V0), 0xAA99887766554433ull);)

TESTCASE(swl_swr, ASM(swr(A1, 0, A0), swl(A1, 3, A0)),
	GPR(A0) = 0x4001; GPR(A1) = 0xA1B2C3D4;
	RUN();
	CHECK_EQ(r64(0x4000), 0x000000A1B2C3D400ull);)

TESTCASE(sdl_sdr, ASM(sdr(A1, 0, A0), sdl(A1, 7, A0)),
	GPR(A0) = 0x5003; GPR(A1) = 0x0102030405060708ull;
	RUN();
	CHECK_EQ(r64(0x5000), 0x0405060708000000ull);
	CHECK_EQ(r64(0x5008), 0x0000000000010203ull);)

TESTCASE(unaligned_partial, ASM(ldr(V1, 0, A0), ldl(T0, 0, A1), sdr(A3, 0, A2), sdl(A3, 0, T1), swr(A3, 0, T2), swl(A3, 0, T3)),
	w64(0x3000, 0x7766554433221100ull);
	for (uint32_t k = 0; k < 4; k++) w64(0x6000 + 8 * k, ~0ull);
	GPR(A0) = 0x3003; GPR(A1) = 0x3002; GPR(V1) = GPR(T0) = 0x1122334455667788ull; GPR(A3) = 0x0102030405060708ull;
	GPR(A2) = 0x6003; GPR(T1) = 0x600A; GPR(T2) = 0x6011; GPR(T3) = 0x6016;
	RUN();
	CHECK_EQ(GPR(V1), 0x1122337766554433ull); // ldr keeps the high bytes
	CHECK_EQ(GPR(T0), 0x2211004455667788ull); // ldl keeps the low bytes
	CHECK_EQ(r64(0x6000), 0x0405060708FFFFFFull);
	CHECK_EQ(r64(0x6008), 0xFFFFFFFFFF010203ull);
	CHECK_EQ(r64(0x6010), 0xFF050607060708FFull);)

TESTCASE(scratchpad, ASM(sw(A1, 0, A0), lw(V0, 0, A0), sq(A2, 0x10, A0), lq(V1, 0x10, A0)),
	GPR(A0) = 0x70000010; GPR(A1) = 0xCAFEBABE; set128(c->r[A2], 1, 2);
	RUN();
	uint32_t s; std::memcpy(&s, g_spr + 0x10, 4);
	CHECK_EQ(s, 0xCAFEBABE);
	CHECK_EQ(GPR(V0), 0xFFFFFFFFCAFEBABEull);
	CHECK_EQ(GPR(V1), 1); CHECK_EQ(GPRHI(V1), 2);)

TESTCASE(slow_path, ASM(sw(A1, 0, A0), lw(V0, 0, A2), lw(V1, 0, A3), sw(A1, 0x10, A3)),
	GPR(A0) = 0x20001000; GPR(A1) = 0x12345678; GPR(A2) = 0x1000; GPR(A3) = 0x10003000;
	RUN();
	CHECK_EQ(GPR(V0), 0x12345678); // uncached mirror reaches RAM
	CHECK_EQ(GPR(V1), 0x4A5A6A5A); // I/O read via runtime slow path
	CHECK_EQ(g_log.io_last_write_addr, 0x10003010);
	CHECK_EQ(g_log.io_last_write_val, 0x12345678);)

// ======================= branches / delay slots =======================
TESTCASE(beq_bne, ASM(beq(A0, A1, 2), addiu(V0, V0, 1), addiu(V1, V1, 1), bne(A0, A1, 2), addiu(T0, T0, 1), addiu(T1, T1, 1)),
	GPR(A0) = 5; GPR(A1) = 5;
	RUN();
	CHECK_EQ(GPR(V0), 1); CHECK_EQ(GPR(V1), 0); CHECK_EQ(GPR(T0), 1); CHECK_EQ(GPR(T1), 1);
	GPR(V0) = GPR(V1) = GPR(T0) = GPR(T1) = 0; GPR(A0) = 0x100000005ull; // differs only above bit 31
	RUN();
	CHECK_EQ(GPR(V0), 1); CHECK_EQ(GPR(V1), 1); CHECK_EQ(GPR(T0), 1); CHECK_EQ(GPR(T1), 0);)

TESTCASE(branch_zero_cmp, ASM(blez(A0, 2), addiu(V0, V0, 1), addiu(V0, V0, 2), bgtz(A0, 2), addiu(V1, V1, 1), addiu(V1, V1, 2),
                              bltz(A0, 2), addiu(T0, T0, 1), addiu(T0, T0, 2), bgez(A0, 2), addiu(T1, T1, 1), addiu(T1, T1, 2)),
	const int64_t vals[3] = {-5, 0, 5};
	const uint64_t want[3][4] = {{1, 3, 1, 3}, {1, 3, 3, 1}, {3, 1, 3, 1}};
	for (int k = 0; k < 3; k++) {
		GPR(A0) = static_cast<uint64_t>(vals[k]); GPR(V0) = GPR(V1) = GPR(T0) = GPR(T1) = 0;
		RUN();
		CHECK_EQ(GPR(V0), want[k][0]); CHECK_EQ(GPR(V1), want[k][1]); CHECK_EQ(GPR(T0), want[k][2]); CHECK_EQ(GPR(T1), want[k][3]);
	})

TESTCASE(branch_likely, ASM(beql(A0, A1, 2), addiu(V0, V0, 1), addiu(V0, V0, 2), bnel(A0, A1, 2), addiu(V1, V1, 1), addiu(V1, V1, 2),
                            blezl(A0, 2), addiu(T0, T0, 1), addiu(T0, T0, 2), bgtzl(A0, 2), addiu(T1, T1, 1), addiu(T1, T1, 2),
                            bltzl(A0, 2), addiu(T2, T2, 1), addiu(T2, T2, 2), bgezl(A0, 2), addiu(T3, T3, 1), addiu(T3, T3, 2)),
	// taken: delay slot runs, skip insn (=1). not taken: delay slot annulled (=2)
	GPR(A0) = 5; GPR(A1) = 5;
	RUN();
	CHECK_EQ(GPR(V0), 1); CHECK_EQ(GPR(V1), 2); CHECK_EQ(GPR(T0), 2); CHECK_EQ(GPR(T1), 1); CHECK_EQ(GPR(T2), 2); CHECK_EQ(GPR(T3), 1);
	GPR(A0) = static_cast<uint64_t>(-1ll); GPR(V0) = GPR(V1) = GPR(T0) = GPR(T1) = GPR(T2) = GPR(T3) = 0;
	RUN();
	CHECK_EQ(GPR(V0), 2); CHECK_EQ(GPR(V1), 1); CHECK_EQ(GPR(T0), 1); CHECK_EQ(GPR(T1), 2); CHECK_EQ(GPR(T2), 1); CHECK_EQ(GPR(T3), 2);)

TESTCASE(branch_always, ASM(beq(ZERO, ZERO, 2), addiu(V0, V0, 1), addiu(V1, V1, 1), addiu(T0, T0, 1), bne(ZERO, ZERO, 1), addiu(T1, T1, 1), addiu(T2, T2, 1),
                            bnel(ZERO, ZERO, 1), addiu(T3, T3, 1), addiu(T4, T4, 1)),
	RUN();
	CHECK_EQ(GPR(V0), 1); CHECK_EQ(GPR(V1), 0); CHECK_EQ(GPR(T0), 1);
	CHECK_EQ(GPR(T1), 1); CHECK_EQ(GPR(T2), 1); // never-taken bne: delay runs
	CHECK_EQ(GPR(T3), 0); CHECK_EQ(GPR(T4), 1);) // never-taken bnel: delay annulled

TESTCASE(loop_countdown, ASM(addiu(A0, ZERO, 10), addiu(A0, A0, -1), bne(A0, ZERO, -2), addiu(V0, V0, 2)),
	RUN();
	CHECK_EQ(GPR(A0), 0);
	CHECK_EQ(GPR(V0), 20);)

TESTCASE(delay_writes_cond_reg, ASM(beq(A0, ZERO, 2), addiu(A0, A0, 1), addiu(V0, ZERO, 7), nop()),
	GPR(A0) = 0;
	RUN();
	CHECK_EQ(GPR(A0), 1); // condition evaluated before the delay slot
	CHECK_EQ(GPR(V0), 0);)

TESTCASE(branch_into_delay_slot, ASM(bne(A0, ZERO, 2), addiu(V0, V0, 1), addiu(V1, V1, 1), beq(A1, ZERO, -3), addiu(A1, A1, 1), nop()),
	GPR(A0) = 1; GPR(A1) = 0;
	RUN();
	CHECK_EQ(GPR(V0), 2); CHECK_EQ(GPR(V1), 1); CHECK_EQ(GPR(A1), 2);)

TESTCASE(jr_ra_delay, ASM(jr(RA), addiu(V0, ZERO, 4), addiu(V0, ZERO, 99)),
	RUN();
	CHECK_EQ(GPR(V0), 4);)

// ======================= calls =======================
TESTCASE(jal_override, ASM(jal(0x00300000), addiu(A0, ZERO, 3), addu(V1, V0, A0)),
	RUN();
	CHECK_EQ(g_log.callee1_hits, 1); // runtime's strong definition replaced the weak one
	CHECK_EQ(g_log.callee1_a0, 3);   // delay slot ran before the call
	CHECK_EQ(g_log.callee1_ra, TADDR_jal_override + 8);
	CHECK_EQ(GPR(RA), TADDR_jal_override + 8);
	CHECK_EQ(GPR(V1), 0xCA14);)

TESTCASE(jal_recompiled, ASM(jal(0x00300100), addiu(V0, ZERO, 10), addiu(V0, V0, 1)),
	RUN();
	CHECK_EQ(GPR(V0), 16);)

TESTCASE(jalr_dispatch, ASM(jalr(RA, A0), addiu(V0, ZERO, 1), jalr(T0, A0), nop()),
	GPR(A0) = 0x00300100;
	RUN();
	CHECK_EQ(GPR(V0), 11);
	CHECK_EQ(GPR(RA), TADDR_jalr_dispatch + 8);
	CHECK_EQ(GPR(T0), TADDR_jalr_dispatch + 16);
	CHECK_EQ(g_log.indirect_calls.size(), 2);
	CHECK_EQ(g_log.indirect_calls[0], 0x00300100);)

TESTCASE(jr_dispatch_tail, ASM(jr(A0), addiu(V0, ZERO, 2), addiu(V0, ZERO, 99)),
	GPR(A0) = 0x00300100;
	RUN();
	CHECK_EQ(GPR(V0), 7);
	CHECK_EQ(g_log.indirect_jumps.size(), 1);
	CHECK_EQ(g_log.indirect_jumps[0], 0x00300100);)

TESTCASE(j_tailcall, ASM(j(0x00300100), addiu(V0, ZERO, 1), addiu(V0, ZERO, 99)),
	RUN();
	CHECK_EQ(GPR(V0), 6);)

TESTCASE(regimm_calls, ASM(bgezal(ZERO, 0x7F), addiu(V0, ZERO, 1), bltzal(A0, 0x7F - 2), addiu(V1, ZERO, 1)),
	GPR(A0) = 1;
	RUN();
	CHECK_EQ(GPR(V0), 6); // bal: always calls
	CHECK_EQ(GPR(V1), 1); // bltzal not taken; link still written
	CHECK_EQ(GPR(RA), TADDR_regimm_calls + 16);
	GPR(A0) = static_cast<uint64_t>(-1ll);
	RUN();
	CHECK_EQ(GPR(V0), 11);) // second bltzal taken: v0 = 1 + 5 (first) then +5

TESTCASE(regimm_calls_likely, ASM(bgezall(A0, 0x7F), addiu(V1, V1, 1), bltzall(A0, 0x7F - 2), addiu(T0, T0, 1)),
	GPR(A0) = static_cast<uint64_t>(-1ll);
	RUN();
	CHECK_EQ(GPR(V1), 0); // bgezall not taken: delay annulled
	CHECK_EQ(GPR(T0), 1);
	CHECK_EQ(GPR(V0), 5);
	CHECK_EQ(GPR(RA), TADDR_regimm_calls_likely + 16);)

TESTCASE_JT(jump_table, 0x7000, OFFS(8, 10, 12),
            ASM(sltiu(AT, A0, 3), beq(AT, ZERO, 12), sll(T0, A0, 2), lui(AT, 0), addu(AT, AT, T0), lw(T0, 0x7000, AT), jr(T0), nop(),
                jr(RA), addiu(V0, ZERO, 100), jr(RA), addiu(V0, ZERO, 101), jr(RA), addiu(V0, ZERO, 102), addiu(V0, ZERO, 99)),
	const uint64_t want[4] = {100, 101, 102, 99};
	const uint64_t idx[4] = {0, 1, 2, 5};
	for (int k = 0; k < 4; k++) {
		GPR(A0) = idx[k]; GPR(V0) = 0;
		RUN();
		CHECK_EQ(GPR(V0), want[k]);
	}
	CHECK_EQ(g_log.indirect_jumps.size(), 0);) // resolved statically, no dispatch

TESTCASE(bc1_branches, ASM(fs_(0x32, 0, 0, 1), bc1t(2), addiu(V0, V0, 1), addiu(V1, V1, 1), bc1f(2), addiu(T0, T0, 1), addiu(T1, T1, 1),
                           bc1tl(2), addiu(T2, T2, 1), addiu(T3, T3, 1), bc1fl(2), addiu(T4, T4, 1), addiu(T5, T5, 1)),
	FPR(0) = fb(1.0f); FPR(1) = fb(1.0f);
	RUN(); // C = 1
	CHECK_EQ(GPR(V0), 1); CHECK_EQ(GPR(V1), 0); CHECK_EQ(GPR(T0), 1); CHECK_EQ(GPR(T1), 1);
	CHECK_EQ(GPR(T2), 1); CHECK_EQ(GPR(T3), 0); CHECK_EQ(GPR(T4), 0); CHECK_EQ(GPR(T5), 1);)

TESTCASE(bc0_bc2_branches, ASM(bc0t(2), addiu(V0, V0, 1), addiu(V1, V1, 1), bc2f(2), addiu(T0, T0, 1), addiu(T1, T1, 1),
                               bc0fl(2), addiu(T2, T2, 1), addiu(T3, T3, 1), bc2tl(2), addiu(T4, T4, 1), addiu(T5, T5, 1)),
	g_log.cop0_cond = 1; g_log.cop2_cond = 1;
	RUN();
	CHECK_EQ(GPR(V0), 1); CHECK_EQ(GPR(V1), 0); CHECK_EQ(GPR(T0), 1); CHECK_EQ(GPR(T1), 1);
	CHECK_EQ(GPR(T2), 0); CHECK_EQ(GPR(T3), 1); CHECK_EQ(GPR(T4), 1); CHECK_EQ(GPR(T5), 0);)

TESTCASE(bc0_bc2_false, ASM(bc0f(2), addiu(V0, V0, 1), addiu(V1, V1, 1), bc0tl(2), addiu(T0, T0, 1), addiu(T1, T1, 1),
                            bc2t(2), addiu(T2, T2, 1), addiu(T3, T3, 1), bc2fl(2), addiu(T4, T4, 1), addiu(T5, T5, 1)),
	g_log.cop0_cond = 0; g_log.cop2_cond = 0;
	RUN();
	CHECK_EQ(GPR(V0), 1); CHECK_EQ(GPR(V1), 0); CHECK_EQ(GPR(T0), 0); CHECK_EQ(GPR(T1), 1);
	CHECK_EQ(GPR(T2), 1); CHECK_EQ(GPR(T3), 1); CHECK_EQ(GPR(T4), 1); CHECK_EQ(GPR(T5), 0);)

// ======================= system / COP0 =======================
TESTCASE(syscall_break, ASM(addiu(V1, ZERO, 0x3C), syscall_(0x12), break_(7)),
	RUN();
	CHECK_EQ(g_log.syscalls, 1);
	CHECK_EQ(g_log.syscall_v1, 0x3C);
	CHECK_EQ(GPR(V0), 0x5C);
	CHECK_EQ(g_log.breaks, 1);
	CHECK_EQ(g_log.last_code, 7);
	CHECK_EQ(g_log.last_pc, TADDR_syscall_break + 8);)

TESTCASE(traps, ASM(teq(A0, A1), tne(A0, A1), tge(A0, A1), tgeu(A0, A1), tlt(A0, A1), tltu(A0, A1),
                    teqi(A0, -1), tnei(A0, -1), tgei(A0, 0), tgeiu(A0, 0), tlti(A0, 0), tltiu(A1, -1)),
	GPR(A0) = ~0ull; GPR(A1) = 1;
	RUN();
	CHECK_EQ(g_log.traps, 7);)

TESTCASE(cop0, ASM(mfc0(V0, 9), mtc0(A0, 12), ei(), di(), mfc0(V1, 25, 3)),
	GPR(A0) = 0x70030C13;
	RUN();
	CHECK_EQ(GPR(V0), 0xFFFFFFFF80000900ull);
	CHECK_EQ(GPR(V1), 0xFFFFFFFF80001903ull);
	CHECK_EQ(g_log.last_reg, 12);
	CHECK_EQ(g_log.last_value, 0x70030C13);
	CHECK_EQ(g_log.cop0_ops, 2);)

TESTCASE(cop0_tlb_eret, ASM(c0op(0x01), tlbwi(), c0op(0x06), c0op(0x08), addiu(V0, ZERO, 1), eret(), addiu(V0, ZERO, 99)),
	RUN();
	CHECK_EQ(g_log.cop0_ops, 5);          // tlbr, tlbwi, tlbwr, tlbp, eret go to the runtime hook
	CHECK_EQ(g_log.last_insn, eret());
	CHECK_EQ(GPR(V0), 1);)                // eret ends the function

TESTCASE(cache_sync_pref, ASM(cache(0x18, 0x40, A0), sync_(), pref(0, 0, A0), addiu(V0, ZERO, 1)),
	GPR(A0) = 0x1000;
	RUN();
	CHECK_EQ(g_log.caches, 1);
	CHECK_EQ(g_log.last_code, 0x18);
	CHECK_EQ(g_log.last_addr, 0x1040);
	CHECK_EQ(GPR(V0), 1);)

TESTCASE(invalid_opcode, ASM(0xEC000000u, addiu(V0, ZERO, 1)),
	RUN();
	CHECK_EQ(g_log.unimpl, 1);
	CHECK_EQ(g_log.last_insn, 0xEC000000u);
	CHECK_EQ(GPR(V0), 1);)

TESTCASE(r0_never_written, ASM(addiu(ZERO, ZERO, 5), lui(ZERO, 1), lq(ZERO, 0, A0), mmi0(0, ZERO, A1, A1), mflo(ZERO), qmfc2(ZERO, 1)),
	GPR(A0) = 0x1000; GPR(A1) = 3; w64(0x1000, 0x1234);
	RUN();) // runner checks r0 after every test

// ======================= MMI =======================
TESTCASE(mmi_word_arith, ASM(mmi0(0, V0, A0, A1), mmi0(1, V1, A0, A1), mmi0(2, T0, A0, A1), mmi0(3, T1, A0, A1),
                             mmi1(3, T2, A0, A1), mmi1(2, T3, A0, A1), mmi1(2, T4, A0, A0)),
	setw(c->r[A0], 1, 0xFFFFFFFF, 0x7FFFFFFF, 10); setw(c->r[A1], 2, 1, 1, 0xFFFFFFF6);
	RUN();
	auto W = [&](int r, int k) { return c->r[r].uw[k]; };
	CHECK_EQ(W(V0, 0), 3); CHECK_EQ(W(V0, 1), 0); CHECK_EQ(W(V0, 2), 0x80000000); CHECK_EQ(W(V0, 3), 0);           // paddw
	CHECK_EQ(W(V1, 0), 0xFFFFFFFF); CHECK_EQ(W(V1, 1), 0xFFFFFFFE); CHECK_EQ(W(V1, 2), 0x7FFFFFFE); CHECK_EQ(W(V1, 3), 0x14); // psubw
	CHECK_EQ(W(T0, 0), 0); CHECK_EQ(W(T0, 1), 0); CHECK_EQ(W(T0, 2), 0xFFFFFFFF); CHECK_EQ(W(T0, 3), 0xFFFFFFFF); // pcgtw
	CHECK_EQ(W(T1, 0), 2); CHECK_EQ(W(T1, 1), 1); CHECK_EQ(W(T1, 2), 0x7FFFFFFF); CHECK_EQ(W(T1, 3), 10);          // pmaxw
	CHECK_EQ(W(T2, 0), 1); CHECK_EQ(W(T2, 1), 0xFFFFFFFF); CHECK_EQ(W(T2, 2), 1); CHECK_EQ(W(T2, 3), 0xFFFFFFF6);  // pminw
	CHECK_EQ(GPR(T3), 0); CHECK_EQ(GPRHI(T3), 0);                                                                  // pceqw
	CHECK_EQ(GPR(T4), ~0ull); CHECK_EQ(GPRHI(T4), ~0ull);)

TESTCASE(mmi_word_sat, ASM(mmi0(16, V0, A2, A3), mmi0(17, V1, A2, A3), mmi0(17, T1, T5, T6), mmi1(16, T2, A0, A1), mmi1(17, T3, A0, A1), mmi1(1, T4, 0, A2)),
	setw(c->r[A0], 1, 0xFFFFFFFF, 0x7FFFFFFF, 10); setw(c->r[A1], 2, 1, 1, 0xFFFFFFF6);
	setw(c->r[A2], 0x80000000, 0x7FFFFFFF, 5, 0xFFFFFFFB); setw(c->r[A3], 0xFFFFFFFF, 1, 3, 0xFFFFFFFD);
	setw(c->r[T5], 0x7FFFFFFF, 0x80000000, 0, 0); setw(c->r[T6], 0xFFFFFFFF, 1, 0, 0);
	RUN();
	auto W = [&](int r, int k) { return c->r[r].uw[k]; };
	CHECK_EQ(W(V0, 0), 0x80000000); CHECK_EQ(W(V0, 1), 0x7FFFFFFF); CHECK_EQ(W(V0, 2), 8); CHECK_EQ(W(V0, 3), 0xFFFFFFF8); // paddsw
	CHECK_EQ(W(V1, 0), 0x80000001); CHECK_EQ(W(V1, 1), 0x7FFFFFFE); CHECK_EQ(W(V1, 2), 2); CHECK_EQ(W(V1, 3), 0xFFFFFFFE); // psubsw
	CHECK_EQ(W(T1, 0), 0x7FFFFFFF); CHECK_EQ(W(T1, 1), 0x80000000);
	CHECK_EQ(W(T2, 0), 3); CHECK_EQ(W(T2, 1), 0xFFFFFFFF); CHECK_EQ(W(T2, 2), 0x80000000); CHECK_EQ(W(T2, 3), 0xFFFFFFFF); // padduw
	CHECK_EQ(W(T3, 0), 0); CHECK_EQ(W(T3, 1), 0xFFFFFFFE); CHECK_EQ(W(T3, 2), 0x7FFFFFFE); CHECK_EQ(W(T3, 3), 0);          // psubuw
	CHECK_EQ(W(T4, 0), 0x7FFFFFFF); CHECK_EQ(W(T4, 1), 0x7FFFFFFF); CHECK_EQ(W(T4, 2), 5); CHECK_EQ(W(T4, 3), 5);)         // pabsw

TESTCASE(mmi_half_arith, ASM(mmi0(4, V0, A0, A1), mmi0(5, V1, A0, A1), mmi0(6, T0, A0, A1), mmi0(7, T1, A0, A1), mmi1(7, T2, A0, A1),
                             mmi0(20, T3, A0, A1), mmi0(21, T4, A0, A1), mmi1(6, T5, A0, A1), mmi1(4, T6, A0, A1), mmi1(5, T7, 0, A0),
                             mmi0(21, S0, S1, S2)),
	seth(c->r[A0], 1, -1, 0x7FFF, -0x8000, 100, -100, 0, 5); seth(c->r[A1], 2, 1, 1, -1, 200, 50, 0, -5);
	seth(c->r[S1], 0x7FFF, -0x8000, 0, 0, 0, 0, 0, 0); seth(c->r[S2], -1, 1, 0, 0, 0, 0, 0, 0);
	RUN();
	auto H = [&](int r, std::initializer_list<int> e) { int k = 0; for (int v : e) { CHECK_EQ(c->r[r].uh[k], static_cast<uint16_t>(v)); k++; } };
	H(V0, {3, 0, 0x8000, 0x7FFF, 300, -50, 0, 0});              // paddh (wraps)
	H(V1, {-1, -2, 0x7FFE, 0x8001, -100, -150, 0, 10});         // psubh
	H(T0, {0, 0, 0xFFFF, 0, 0, 0, 0, 0xFFFF});                   // pcgth
	H(T1, {2, 1, 0x7FFF, -1, 200, 50, 0, 5});                    // pmaxh
	H(T2, {1, -1, 1, -0x8000, 100, -100, 0, -5});                // pminh
	H(T3, {3, 0, 0x7FFF, 0x8000, 300, -50, 0, 0});               // paddsh
	H(T4, {-1, -2, 0x7FFE, 0x8001, -100, -150, 0, 10});          // psubsh
	H(T5, {0, 0, 0, 0, 0, 0, 0xFFFF, 0});                        // pceqh
	H(T6, {-1, -2, 0x7FFE, 0x8001, 300, -50, 0, 0});             // padsbh
	H(T7, {1, 1, 0x7FFF, 0x7FFF, 100, 100, 0, 5});               // pabsh
	H(S0, {0x7FFF, 0x8000, 0, 0, 0, 0, 0, 0});)                  // psubsh saturation

TESTCASE(mmi_half_unsigned, ASM(mmi1(20, V0, A0, A1), mmi1(21, V1, A0, A1)),
	seth(c->r[A0], 1, -1, 0x7FFF, -0x8000, 100, -100, 0, 5); seth(c->r[A1], 2, 1, 1, -1, 200, 50, 0, -5);
	RUN();
	auto H = [&](int r, std::initializer_list<int> e) { int k = 0; for (int v : e) { CHECK_EQ(c->r[r].uh[k], static_cast<uint16_t>(v)); k++; } };
	H(V0, {3, 0xFFFF, 0x8000, 0xFFFF, 300, 0xFFCE, 0, 0xFFFF}); // padduh
	H(V1, {0, 0xFFFE, 0x7FFE, 0, 0, 0xFF6A, 0, 0});)            // psubuh

TESTCASE(mmi_bytes, ASM(mmi0(8, V0, A0, A1), mmi0(9, V1, A0, A1), mmi0(10, T0, A0, A1), mmi1(10, T1, A0, A1),
                        mmi0(24, T2, A0, A1), mmi0(25, T3, A0, A1), mmi1(24, T4, A0, A1), mmi1(25, T5, A0, A1)),
	const uint8_t a0[5] = {0x7F, 0x80, 0xFF, 0x01, 0x10}, a1[5] = {0x01, 0xFF, 0x01, 0xFF, 0x20};
	std::memset(&c->r[A0], 0, 16); std::memset(&c->r[A1], 0, 16);
	std::memcpy(&c->r[A0], a0, 5); std::memcpy(&c->r[A1], a1, 5);
	RUN();
	auto B = [&](int r, std::initializer_list<int> e) { int k = 0; for (int v : e) { CHECK_EQ(c->r[r].ub[k], static_cast<uint8_t>(v)); k++; } };
	B(V0, {0x80, 0x7F, 0x00, 0x00, 0x30, 0}); // paddb
	B(V1, {0x7E, 0x81, 0xFE, 0x02, 0xF0, 0}); // psubb
	B(T0, {0xFF, 0, 0, 0xFF, 0, 0});          // pcgtb
	B(T1, {0, 0, 0, 0, 0, 0xFF});             // pceqb
	CHECK_EQ(c->r[T1].ub[15], 0xFF);
	B(T2, {0x7F, 0x80, 0, 0, 0x30});          // paddsb
	B(T3, {0x7E, 0x81, 0xFE, 0x02, 0xF0});    // psubsb
	B(T4, {0x80, 0xFF, 0xFF, 0xFF, 0x30});    // paddub
	B(T5, {0x7E, 0, 0xFE, 0, 0});)            // psubub

TESTCASE(mmi_interleave_words, ASM(mmi0(18, V0, A0, A1), mmi1(18, V1, A0, A1), mmi0(19, T0, A0, A1), mmi2(30, T1, 0, A1), mmi2(31, T2, 0, A1), mmi3(30, T3, 0, A1)),
	setw(c->r[A0], 0xA0, 0xA1, 0xA2, 0xA3); setw(c->r[A1], 0xB0, 0xB1, 0xB2, 0xB3);
	RUN();
	auto W = [&](int r, uint32_t w0, uint32_t w1, uint32_t w2, uint32_t w3) {
		CHECK_EQ(c->r[r].uw[0], w0); CHECK_EQ(c->r[r].uw[1], w1); CHECK_EQ(c->r[r].uw[2], w2); CHECK_EQ(c->r[r].uw[3], w3); };
	W(V0, 0xB0, 0xA0, 0xB1, 0xA1); // pextlw
	W(V1, 0xB2, 0xA2, 0xB3, 0xA3); // pextuw
	W(T0, 0xB0, 0xB2, 0xA0, 0xA2); // ppacw
	W(T1, 0xB2, 0xB1, 0xB0, 0xB3); // pexew
	W(T2, 0xB1, 0xB2, 0xB0, 0xB3); // prot3w
	W(T3, 0xB0, 0xB2, 0xB1, 0xB3);) // pexcw

TESTCASE(mmi_interleave_halves, ASM(mmi0(22, V0, A0, A1), mmi1(22, V1, A0, A1), mmi0(23, T0, A0, A1), mmi2(10, T1, A0, A1), mmi3(10, T2, A0, A1),
                                    mmi2(26, T3, 0, A1), mmi2(27, T4, 0, A1), mmi3(26, T5, 0, A1), mmi3(27, T6, 0, A1)),
	seth(c->r[A0], 0xA0, 0xA1, 0xA2, 0xA3, 0xA4, 0xA5, 0xA6, 0xA7); seth(c->r[A1], 0xB0, 0xB1, 0xB2, 0xB3, 0xB4, 0xB5, 0xB6, 0xB7);
	RUN();
	auto H = [&](int r, std::initializer_list<int> e) { int k = 0; for (int v : e) { CHECK_EQ(c->r[r].uh[k], static_cast<uint16_t>(v)); k++; } };
	H(V0, {0xB0, 0xA0, 0xB1, 0xA1, 0xB2, 0xA2, 0xB3, 0xA3}); // pextlh
	H(V1, {0xB4, 0xA4, 0xB5, 0xA5, 0xB6, 0xA6, 0xB7, 0xA7}); // pextuh
	H(T0, {0xB0, 0xB2, 0xB4, 0xB6, 0xA0, 0xA2, 0xA4, 0xA6}); // ppach
	H(T1, {0xB0, 0xA4, 0xB1, 0xA5, 0xB2, 0xA6, 0xB3, 0xA7}); // pinth
	H(T2, {0xB0, 0xA0, 0xB2, 0xA2, 0xB4, 0xA4, 0xB6, 0xA6}); // pinteh
	H(T3, {0xB2, 0xB1, 0xB0, 0xB3, 0xB6, 0xB5, 0xB4, 0xB7}); // pexeh
	H(T4, {0xB3, 0xB2, 0xB1, 0xB0, 0xB7, 0xB6, 0xB5, 0xB4}); // prevh
	H(T5, {0xB0, 0xB2, 0xB1, 0xB3, 0xB4, 0xB6, 0xB5, 0xB7}); // pexch
	H(T6, {0xB0, 0xB0, 0xB0, 0xB0, 0xB4, 0xB4, 0xB4, 0xB4});) // pcpyh

TESTCASE(mmi_interleave_bytes, ASM(mmi0(26, V0, A0, A1), mmi1(26, V1, A0, A1), mmi0(27, T0, A0, A1)),
	for (int k = 0; k < 16; k++) { c->r[A0].ub[k] = static_cast<uint8_t>(0xA0 + k); c->r[A1].ub[k] = static_cast<uint8_t>(0xB0 + k); }
	RUN();
	CHECK_EQ(c->r[V0].ub[0], 0xB0); CHECK_EQ(c->r[V0].ub[1], 0xA0); CHECK_EQ(c->r[V0].ub[14], 0xB7); CHECK_EQ(c->r[V0].ub[15], 0xA7); // pextlb
	CHECK_EQ(c->r[V1].ub[0], 0xB8); CHECK_EQ(c->r[V1].ub[1], 0xA8); CHECK_EQ(c->r[V1].ub[14], 0xBF); CHECK_EQ(c->r[V1].ub[15], 0xAF); // pextub
	CHECK_EQ(c->r[T0].ub[0], 0xB0); CHECK_EQ(c->r[T0].ub[7], 0xBE); CHECK_EQ(c->r[T0].ub[8], 0xA0); CHECK_EQ(c->r[T0].ub[15], 0xAE);) // ppacb

TESTCASE(mmi_pext5_ppac5, ASM(mmi0(30, V0, 0, A1), mmi0(31, V1, 0, V0)),
	setw(c->r[A1], 0x0000FFFF, 0x00000421, 0x00008000, 0x0000001F);
	RUN();
	CHECK_EQ(c->r[V0].uw[0], 0x80F8F8F8); CHECK_EQ(c->r[V0].uw[1], 0x00080808); CHECK_EQ(c->r[V0].uw[2], 0x80000000); CHECK_EQ(c->r[V0].uw[3], 0xF8);
	CHECK_EQ(c->r[V1].uw[0], 0xFFFF); CHECK_EQ(c->r[V1].uw[1], 0x0421); CHECK_EQ(c->r[V1].uw[2], 0x8000); CHECK_EQ(c->r[V1].uw[3], 0x1F);)

TESTCASE(mmi_copy_logic, ASM(mmi2(14, V0, A0, A1), mmi3(14, V1, A0, A1), mmi2(18, T0, A0, A1), mmi3(18, T1, A0, A1), mmi2(19, T2, A0, A1), mmi3(19, T3, A0, A1),
                             mmi2(14, A0, A0, A0)),
	set128(c->r[A0], 0xFF00FF00FF00FF00ull, 0x1111111111111111ull); set128(c->r[A1], 0x0FF00FF00FF00FF0ull, 0x2222222222222222ull);
	RUN();
	CHECK_EQ(GPR(V0), 0x0FF00FF00FF00FF0ull); CHECK_EQ(GPRHI(V0), 0xFF00FF00FF00FF00ull); // pcpyld
	CHECK_EQ(GPR(V1), 0x1111111111111111ull); CHECK_EQ(GPRHI(V1), 0x2222222222222222ull); // pcpyud
	CHECK_EQ(GPR(T0), 0x0F000F000F000F00ull); CHECK_EQ(GPRHI(T0), 0);                    // pand
	CHECK_EQ(GPR(T1), 0xFFF0FFF0FFF0FFF0ull); CHECK_EQ(GPRHI(T1), 0x3333333333333333ull); // por
	CHECK_EQ(GPR(T2), 0xF0F0F0F0F0F0F0F0ull); CHECK_EQ(GPRHI(T2), 0x3333333333333333ull); // pxor
	CHECK_EQ(GPR(T3), 0x000F000F000F000Full); CHECK_EQ(GPRHI(T3), 0xCCCCCCCCCCCCCCCCull); // pnor
	CHECK_EQ(GPR(A0), 0xFF00FF00FF00FF00ull); CHECK_EQ(GPRHI(A0), 0xFF00FF00FF00FF00ull);) // rd aliases rs/rt

TESTCASE(mmi_shifts_imm, ASM(psllh(V0, A1, 4), psrlh(V1, A1, 4), psrah(T0, A1, 4), psllw(T1, A2, 4), psrlw(T2, A2, 4), psraw(T3, A2, 4)),
	seth(c->r[A1], 0x1234, 0x8001, 0, 0, 0, 0, 0, 0xFFFF); setw(c->r[A2], 0x12345678, 0x80000001, 0, 0xFFFFFFFF);
	RUN();
	CHECK_EQ(c->r[V0].uh[0], 0x2340); CHECK_EQ(c->r[V0].uh[1], 0x0010); CHECK_EQ(c->r[V0].uh[7], 0xFFF0);
	CHECK_EQ(c->r[V1].uh[0], 0x0123); CHECK_EQ(c->r[V1].uh[1], 0x0800); CHECK_EQ(c->r[V1].uh[7], 0x0FFF);
	CHECK_EQ(c->r[T0].uh[0], 0x0123); CHECK_EQ(c->r[T0].uh[1], 0xF800); CHECK_EQ(c->r[T0].uh[7], 0xFFFF);
	CHECK_EQ(c->r[T1].uw[0], 0x23456780); CHECK_EQ(c->r[T1].uw[1], 0x00000010); CHECK_EQ(c->r[T1].uw[3], 0xFFFFFFF0);
	CHECK_EQ(c->r[T2].uw[0], 0x01234567); CHECK_EQ(c->r[T2].uw[1], 0x08000000); CHECK_EQ(c->r[T2].uw[3], 0x0FFFFFFF);
	CHECK_EQ(c->r[T3].uw[0], 0x01234567); CHECK_EQ(c->r[T3].uw[1], 0xF8000000); CHECK_EQ(c->r[T3].uw[3], 0xFFFFFFFF);)

TESTCASE(mmi_shifts_var, ASM(mmi2(2, V0, A0, A1), mmi2(3, V1, A0, A2), mmi3(3, T0, A0, A2)),
	setw(c->r[A0], 1, 0, 33, 0); setw(c->r[A1], 0x40000001, 0, 1, 0); setw(c->r[A2], 0x80000000, 0, 0x80000000, 0);
	RUN();
	CHECK_EQ(GPR(V0), 0xFFFFFFFF80000002ull); CHECK_EQ(GPRHI(V0), 2);                    // psllvw
	CHECK_EQ(GPR(V1), 0x40000000); CHECK_EQ(GPRHI(V1), 0x40000000);                      // psrlvw
	CHECK_EQ(GPR(T0), 0xFFFFFFFFC0000000ull); CHECK_EQ(GPRHI(T0), 0xFFFFFFFFC0000000ull);) // psravw

TESTCASE(mmi_plzcw, ASM(plzcw(V0, A0), plzcw(V1, A1)),
	setw(c->r[A0], 1, 0xFFFFFFFF, 7, 7); setw(c->r[A1], 0x40000000, 0, 0, 0); setw(c->r[V0], 9, 9, 9, 9);
	RUN();
	CHECK_EQ(c->r[V0].uw[0], 30); CHECK_EQ(c->r[V0].uw[1], 31); CHECK_EQ(c->r[V0].uw[2], 9);
	CHECK_EQ(c->r[V1].uw[0], 0); CHECK_EQ(c->r[V1].uw[1], 31);)

TESTCASE(mmi_hilo_moves, ASM(mmi3(8, 0, A0, 0), mmi3(9, 0, A1, 0), mmi2(8, V0, 0, 0), mmi2(9, V1, 0, 0)),
	set128(c->r[A0], 1, 2); set128(c->r[A1], 3, 4);
	RUN();
	CHECK_EQ(c->hi.ud[0], 1); CHECK_EQ(c->hi.ud[1], 2); CHECK_EQ(c->lo.ud[0], 3); CHECK_EQ(c->lo.ud[1], 4);
	CHECK_EQ(GPR(V0), 1); CHECK_EQ(GPRHI(V0), 2); CHECK_EQ(GPR(V1), 3); CHECK_EQ(GPRHI(V1), 4);)

TESTCASE(mmi_pmfhl, ASM(pmfhl(V0, 0), pmfhl(V1, 1), pmfhl(T0, 2), pmfhl(T1, 3)),
	setw(c->lo, 0x10, 0x11, 0xFFFFFFFE, 0x13); setw(c->hi, 0x1, 0x21, 0xFFFFFFFF, 0x23);
	RUN();
	CHECK_EQ(c->r[V0].uw[0], 0x10); CHECK_EQ(c->r[V0].uw[1], 0x1); CHECK_EQ(c->r[V0].uw[2], 0xFFFFFFFE); CHECK_EQ(c->r[V0].uw[3], 0xFFFFFFFF); // .lw
	CHECK_EQ(c->r[V1].uw[0], 0x11); CHECK_EQ(c->r[V1].uw[1], 0x21); CHECK_EQ(c->r[V1].uw[2], 0x13); CHECK_EQ(c->r[V1].uw[3], 0x23);           // .uw
	CHECK_EQ(GPR(T0), 0x7FFFFFFF); CHECK_EQ(GPRHI(T0), 0xFFFFFFFFFFFFFFFEull);                                                              // .slw
	CHECK_EQ(c->r[T1].uh[0], 0x10); CHECK_EQ(c->r[T1].uh[1], 0x11); CHECK_EQ(c->r[T1].uh[2], 0x1); CHECK_EQ(c->r[T1].uh[3], 0x21);         // .lh
	CHECK_EQ(c->r[T1].uh[4], 0xFFFE); CHECK_EQ(c->r[T1].uh[6], 0xFFFF);)

TESTCASE(mmi_pmfhl_sh_pmthl, ASM(pmfhl(V0, 4), pmthl(A0, 0)),
	setw(c->lo, 0x00010000, static_cast<uint32_t>(-70000), 5, static_cast<uint32_t>(-5)); setw(c->hi, 100, 0x7FFF, static_cast<uint32_t>(-0x8000), 0x8000);
	setw(c->r[A0], 0xA, 0xB, 0xC, 0xD);
	RUN();
	const uint16_t want[8] = {0x7FFF, 0x8000, 100, 0x7FFF, 5, 0xFFFB, 0x8000, 0x7FFF};
	for (int k = 0; k < 8; k++) CHECK_EQ(c->r[V0].uh[k], want[k]);
	CHECK_EQ(c->lo.uw[0], 0xA); CHECK_EQ(c->hi.uw[0], 0xB); CHECK_EQ(c->lo.uw[2], 0xC); CHECK_EQ(c->hi.uw[2], 0xD);
	CHECK_EQ(c->lo.uw[1], static_cast<uint32_t>(-70000)); CHECK_EQ(c->hi.uw[3], 0x8000);) // other words untouched

TESTCASE(mmi_word_mul, ASM(mmi2(12, V0, A0, A1), mmi3(12, V1, A2, A3)),
	setw(c->r[A0], static_cast<uint32_t>(-2), 0, 0x10000, 0); setw(c->r[A1], 3, 0, 0x10000, 0);
	setw(c->r[A2], 0xFFFFFFFF, 0, 3, 0); setw(c->r[A3], 2, 0, 4, 0);
	RUN();
	CHECK_EQ(GPR(V0), static_cast<uint64_t>(-6ll)); CHECK_EQ(GPRHI(V0), 0x100000000ull); // pmultw
	CHECK_EQ(GPR(V1), 0x1FFFFFFFEull); CHECK_EQ(GPRHI(V1), 12);                       // pmultuw
	CHECK_EQ(c->lo.ud[0], 0xFFFFFFFFFFFFFFFEull); CHECK_EQ(c->hi.ud[0], 1);
	CHECK_EQ(c->lo.ud[1], 12); CHECK_EQ(c->hi.ud[1], 0);)

TESTCASE(mmi_word_madd, ASM(mmi2(0, V0, A0, A1), mmi2(4, V1, A0, A1)),
	setw(c->r[A0], static_cast<uint32_t>(-2), 0, 0x10000, 0); setw(c->r[A1], 3, 0, 0x10000, 0);
	setw(c->lo, 10, 0, 0, 0); setw(c->hi, 0, 0, 1, 0);
	RUN();
	CHECK_EQ(GPR(V0), 4); CHECK_EQ(GPRHI(V0), 0x200000000ull);                         // pmaddw
	CHECK_EQ(GPR(V1), 10); CHECK_EQ(GPRHI(V1), 0x100000000ull);                        // pmsubw (4 - -6, 0x2_0000_0000 - 0x1_0000_0000)
	CHECK_EQ(c->lo.ud[0], 10); CHECK_EQ(c->hi.ud[0], 0); CHECK_EQ(c->lo.ud[1], 0); CHECK_EQ(c->hi.ud[1], 1);)

TESTCASE(mmi_pmadduw, ASM(mmi3(0, V0, A0, A1)),
	setw(c->r[A0], 0xFFFFFFFF, 0, 1, 0); setw(c->r[A1], 2, 0, 1, 0); setw(c->lo, 10, 0, 0, 0); setw(c->hi, 0, 0, 0, 0);
	RUN();
	CHECK_EQ(GPR(V0), 0x200000008ull); CHECK_EQ(c->lo.ud[0], 8); CHECK_EQ(c->hi.ud[0], 2); CHECK_EQ(GPRHI(V0), 1);)

TESTCASE(mmi_word_div, ASM(mmi2(13, 0, A0, A1), mmi2(9, V0, 0, 0), mmi2(8, V1, 0, 0), mmi3(13, 0, A2, A3), mmi2(9, T0, 0, 0), mmi2(8, T1, 0, 0)),
	setw(c->r[A0], static_cast<uint32_t>(-7), 0, 10, 0); setw(c->r[A1], 2, 0, 0, 0);
	setw(c->r[A2], 0xFFFFFFFF, 0, 7, 0); setw(c->r[A3], 2, 0, 0, 0);
	RUN();
	CHECK_EQ(GPR(V0), static_cast<uint64_t>(-3ll)); CHECK_EQ(GPRHI(V0), ~0ull); // pdivw quotients
	CHECK_EQ(GPR(V1), ~0ull); CHECK_EQ(GPRHI(V1), 10);                          // remainders
	CHECK_EQ(GPR(T0), 0x7FFFFFFF); CHECK_EQ(GPRHI(T0), ~0ull);                  // pdivuw
	CHECK_EQ(GPR(T1), 1); CHECK_EQ(GPRHI(T1), 7);)

TESTCASE(mmi_pdivbw, ASM(mmi2(29, 0, A0, A1)),
	setw(c->r[A0], 100, static_cast<uint32_t>(-100), 7, 0x80000000); seth(c->r[A1], -3, 0, 0, 0, 0, 0, 0, 0);
	RUN();
	CHECK_EQ(c->lo.sw[0], -33); CHECK_EQ(c->lo.sw[1], 33); CHECK_EQ(c->lo.sw[2], -2); CHECK_EQ(c->lo.uw[3], 0x2AAAAAAA);
	CHECK_EQ(c->hi.sw[0], 1); CHECK_EQ(c->hi.sw[1], -1); CHECK_EQ(c->hi.sw[2], 1); CHECK_EQ(c->hi.sw[3], -2);)

TESTCASE(mmi_half_mul, ASM(mmi2(28, V0, A0, A1)),
	seth(c->r[A0], 1, 2, 3, 4, 5, 6, 7, 8); seth(c->r[A1], 10, -1, 100, 2, -3, 3, 0x7FFF, 0x7FFF);
	RUN();
	CHECK_EQ(c->lo.sw[0], 10); CHECK_EQ(c->lo.sw[1], -2); CHECK_EQ(c->lo.sw[2], -15); CHECK_EQ(c->lo.sw[3], 18);
	CHECK_EQ(c->hi.sw[0], 300); CHECK_EQ(c->hi.sw[1], 8); CHECK_EQ(c->hi.sw[2], 229369); CHECK_EQ(c->hi.sw[3], 262136);
	CHECK_EQ(c->r[V0].sw[0], 10); CHECK_EQ(c->r[V0].sw[1], 300); CHECK_EQ(c->r[V0].sw[2], -15); CHECK_EQ(c->r[V0].sw[3], 229369);)

TESTCASE(mmi_half_madd, ASM(mmi2(16, V0, A0, A1)),
	seth(c->r[A0], 1, 2, 3, 4, 5, 6, 7, 8); seth(c->r[A1], 10, -1, 100, 2, -3, 3, 0x7FFF, 0x7FFF);
	setw(c->lo, 1, 1, 1, 1); setw(c->hi, 2, 2, 2, 2);
	RUN();
	CHECK_EQ(c->lo.sw[0], 11); CHECK_EQ(c->lo.sw[1], -1); CHECK_EQ(c->lo.sw[2], -14); CHECK_EQ(c->lo.sw[3], 19);
	CHECK_EQ(c->hi.sw[0], 302); CHECK_EQ(c->hi.sw[1], 10); CHECK_EQ(c->hi.sw[2], 229371); CHECK_EQ(c->hi.sw[3], 262138);
	CHECK_EQ(c->r[V0].sw[0], 11); CHECK_EQ(c->r[V0].sw[1], 302); CHECK_EQ(c->r[V0].sw[2], -14); CHECK_EQ(c->r[V0].sw[3], 229371);)

TESTCASE(mmi_half_msub, ASM(mmi2(20, V0, A0, A1)),
	seth(c->r[A0], 1, 2, 3, 4, 5, 6, 7, 8); seth(c->r[A1], 10, -1, 100, 2, -3, 3, 0x7FFF, 0x7FFF);
	setw(c->lo, 1, 1, 1, 1); setw(c->hi, 2, 2, 2, 2);
	RUN();
	CHECK_EQ(c->lo.sw[0], -9); CHECK_EQ(c->lo.sw[1], 3); CHECK_EQ(c->lo.sw[2], 16); CHECK_EQ(c->lo.sw[3], -17);
	CHECK_EQ(c->hi.sw[0], -298); CHECK_EQ(c->hi.sw[1], -6); CHECK_EQ(c->hi.sw[2], -229367); CHECK_EQ(c->hi.sw[3], -262134);
	CHECK_EQ(c->r[V0].sw[0], -9); CHECK_EQ(c->r[V0].sw[1], -298); CHECK_EQ(c->r[V0].sw[2], 16); CHECK_EQ(c->r[V0].sw[3], -229367);)

TESTCASE(mmi_half_hmadd, ASM(mmi2(17, V0, A0, A1), mmi2(21, V1, A0, A1)),
	seth(c->r[A0], 1, 2, 3, 4, 5, 6, 7, 8); seth(c->r[A1], 10, -1, 100, 2, -3, 3, 0x7FFF, 0x7FFF);
	RUN(); // LO/HI hold the phmsbh results (second instruction)
	CHECK_EQ(c->r[V0].sw[0], 8); CHECK_EQ(c->r[V0].sw[1], 308); CHECK_EQ(c->r[V0].sw[2], 3); CHECK_EQ(c->r[V0].sw[3], 491505); // phmadh
	CHECK_EQ(c->r[V1].sw[0], -12); CHECK_EQ(c->r[V1].sw[1], -292); CHECK_EQ(c->r[V1].sw[2], 33); CHECK_EQ(c->r[V1].sw[3], 32767); // phmsbh
	CHECK_EQ(c->lo.uw[1], 1u); CHECK_EQ(c->hi.uw[1], 0xFFFFFFF7u); CHECK_EQ(c->lo.uw[3], 0xFFFFFFEDu); CHECK_EQ(c->hi.uw[3], ~262136u);)

// ======================= COP1 (FPU) =======================
TESTCASE(fpu_arith, ASM(fs_(0, 3, 1, 2), fs_(1, 4, 1, 2), fs_(2, 5, 1, 2), fs_(3, 6, 1, 2)),
	FPR(1) = fb(1.5f); FPR(2) = fb(2.25f);
	RUN();
	CHECK_EQ(FPR(3), fb(3.75f));
	CHECK_EQ(FPR(4), fb(-0.75f));
	CHECK_EQ(FPR(5), fb(3.375f));
	CHECK_EQ(FPR(6), 0x3F2AAAAAu);) // 1.5/2.25 rounded toward zero (nearest would be ...AB)

TESTCASE(fpu_round_to_zero, ASM(fs_(0, 3, 1, 2), cvt_s_w(5, 4), cvt_s_w(7, 6)),
	FPR(1) = fb(1.0f); FPR(2) = 0x33C00000u; c->f[4].s = 16777219; c->f[6].s = -7;
	RUN();
	CHECK_EQ(FPR(3), fb(1.0f));     // 1 + 0.75 ulp truncates
	CHECK_EQ(FPR(5), 0x4B800001u);  // 16777218.0 (nearest-even would give 16777220)
	CHECK_EQ(FPR(7), fb(-7.0f));)

TESTCASE(fpu_clamp, ASM(fs_(0, 10, 3, 3), fs_(2, 11, 3, 12), fs_(0, 13, 4, 5), fs_(2, 14, 15, 16), fs_(0, 17, 6, 0), fs_(2, 18, 7, 8)),
	FPR(3) = 0x7F7FFFFFu; FPR(12) = fb(-2.0f); FPR(4) = 0x7F800000u; FPR(5) = 0xFF7FFFFFu; FPR(15) = 0x7FC00000u; FPR(16) = fb(1.0f);
	FPR(6) = 0x00000001u; FPR(0) = 0; FPR(7) = 0x00800000u; FPR(8) = fb(0.5f);
	RUN();
	CHECK_EQ(FPR(10), 0x7F7FFFFFu); // overflow -> +FMAX
	CHECK_EQ(FPR(11), 0xFF7FFFFFu); // -> -FMAX
	CHECK_EQ(FPR(13), 0);           // Inf input acts as FMAX
	CHECK_EQ(FPR(14), 0x7F7FFFFFu); // NaN input acts as FMAX
	CHECK_EQ(FPR(17), 0);           // denormal input -> 0
	CHECK_EQ(FPR(18), 0);)          // denormal result -> 0

TESTCASE(fpu_div_special, ASM(fs_(3, 2, 1, 9), cfc1(V0, 31), ctc1(ZERO, 31), fs_(3, 3, 10, 10), cfc1(V1, 31)),
	FPR(1) = fb(1.5f); FPR(9) = 0x80000000u; FPR(10) = 0;
	RUN();
	CHECK_EQ(FPR(2), 0xFF7FFFFFu);
	CHECK_EQ(GPR(V0) & 0x00010020u, 0x00010020u); // D + SD
	CHECK_EQ(FPR(3), 0x7F7FFFFFu);
	CHECK_EQ(GPR(V1) & 0x00020040u, 0x00020040u);) // 0/0: I + SI

TESTCASE(fpu_sqrt_rsqrt, ASM(fs_(4, 2, 0, 1), fs_(4, 3, 0, 4), cfc1(V0, 31), fs_(4, 5, 0, 6), fs_(0x16, 7, 8, 1), fs_(0x16, 9, 8, 0)),
	FPR(1) = fb(4.0f); FPR(4) = fb(-4.0f); FPR(6) = 0x80000000u; FPR(8) = fb(1.5f); FPR(0) = 0;
	RUN();
	CHECK_EQ(FPR(2), fb(2.0f));
	CHECK_EQ(FPR(3), fb(2.0f)); // sqrt(|x|)
	CHECK_EQ(GPR(V0) & 0x00020000u, 0x00020000u);
	CHECK_EQ(FPR(5), 0x80000000u); // sqrt(-0) = -0
	CHECK_EQ(FPR(7), fb(0.75f));
	CHECK_EQ(FPR(9), 0x7F7FFFFFu);) // rsqrt by zero

TESTCASE(fpu_unary, ASM(fs_(5, 2, 1, 0), fs_(7, 3, 1, 0), fs_(6, 4, 1, 0), fs_(7, 5, 2, 0)),
	FPR(1) = fb(-1.5f);
	RUN();
	CHECK_EQ(FPR(2), fb(1.5f)); CHECK_EQ(FPR(3), fb(1.5f)); CHECK_EQ(FPR(4), fb(-1.5f)); CHECK_EQ(FPR(5), fb(-1.5f));)

TESTCASE(fpu_acc, ASM(fs_(0x18, 0, 1, 2), fs_(0x1C, 10, 3, 4), fs_(0x1D, 11, 3, 4), fs_(0x1E, 0, 3, 4), fs_(0x1C, 12, 20, 20),
                      fs_(0x1F, 0, 1, 2), fs_(0x1C, 13, 20, 20), fs_(0x1A, 0, 3, 4), fs_(0x1C, 14, 20, 20), fs_(0x19, 0, 1, 2), fs_(0x1C, 15, 20, 20)),
	FPR(1) = fb(1.5f); FPR(2) = fb(2.25f); FPR(3) = fb(2.0f); FPR(4) = fb(4.0f); FPR(20) = 0;
	RUN();
	CHECK_EQ(FPR(10), fb(11.75f));  // adda; madd
	CHECK_EQ(FPR(11), fb(-4.25f));  // msub
	CHECK_EQ(FPR(12), fb(11.75f));  // madda
	CHECK_EQ(FPR(13), fb(8.375f));  // msuba
	CHECK_EQ(FPR(14), fb(8.0f));    // mula
	CHECK_EQ(FPR(15), fb(-0.75f));) // suba

TESTCASE(fpu_cvt_w, ASM(fs_(0x24, 11, 1, 0), fs_(0x24, 12, 2, 0), fs_(0x24, 13, 3, 0), fs_(0x24, 14, 4, 0), fs_(0x24, 15, 5, 0)),
	FPR(1) = fb(2.75f); FPR(2) = fb(-2.75f); FPR(3) = fb(3e9f); FPR(4) = fb(-3e9f); FPR(5) = 0x4EFFFFFFu;
	RUN();
	CHECK_EQ(FPR(11), 2); CHECK_EQ(FPR(12), static_cast<uint32_t>(-2)); CHECK_EQ(FPR(13), 0x7FFFFFFFu); CHECK_EQ(FPR(14), 0x80000000u);
	CHECK_EQ(FPR(15), 0x7FFFFF80u);)

TESTCASE(fpu_minmax, ASM(fs_(0x28, 10, 1, 2), fs_(0x28, 11, 3, 4), fs_(0x29, 12, 1, 2), fs_(0x29, 13, 3, 4)),
	FPR(1) = fb(-1.0f); FPR(2) = fb(-2.0f); FPR(3) = fb(3.0f); FPR(4) = fb(-5.0f);
	RUN();
	CHECK_EQ(FPR(10), fb(-1.0f)); CHECK_EQ(FPR(11), fb(3.0f)); CHECK_EQ(FPR(12), fb(-2.0f)); CHECK_EQ(FPR(13), fb(-5.0f));)

TESTCASE(fpu_compare, ASM(fs_(0x32, 0, 1, 2), cfc1(V0, 31), fs_(0x34, 0, 1, 3), cfc1(V1, 31), fs_(0x36, 0, 1, 2), cfc1(T0, 31),
                          fs_(0x34, 0, 3, 1), cfc1(T1, 31), fs_(0x36, 0, 3, 1), cfc1(T3, 31), fs_(0x32, 0, 1, 2), fs_(0x30, 0, 1, 2), cfc1(T2, 31)),
	FPR(1) = fb(1.0f); FPR(2) = fb(1.0f); FPR(3) = fb(2.0f);
	RUN();
	CHECK_EQ(GPR(V0) >> 23 & 1, 1); CHECK_EQ(GPR(V1) >> 23 & 1, 1); CHECK_EQ(GPR(T0) >> 23 & 1, 1);
	CHECK_EQ(GPR(T1) >> 23 & 1, 0); CHECK_EQ(GPR(T3) >> 23 & 1, 0); CHECK_EQ(GPR(T2) >> 23 & 1, 0);)

TESTCASE(fpu_moves, ASM(mfc1(V0, 1), mtc1(A0, 2), cfc1(V1, 0), ctc1(A1, 31), cfc1(T0, 31), ctc1(A1, 5), lwc1(3, 0, A2), swc1(3, 4, A2)),
	FPR(1) = 0xBF800000u; GPR(A0) = 0x123456789ABCDEF0ull; GPR(A1) = 0x00800003; GPR(A2) = 0x1000; w32(0x1000, 0x40490FDB);
	RUN();
	CHECK_EQ(GPR(V0), 0xFFFFFFFFBF800000ull);
	CHECK_EQ(FPR(2), 0x9ABCDEF0u);
	CHECK_EQ(GPR(V1), 0x2E00);
	CHECK_EQ(GPR(T0), 0x00800003);
	CHECK_EQ(FPR(3), 0x40490FDBu);
	CHECK_EQ(r32(0x1004), 0x40490FDBu);)

// ======================= COP2 (VU0 macro mode) =======================
TESTCASE(vu_lqc2_sqc2, ASM(lqc2(1, 0, A0), sqc2(1, 0x10, A0), lqc2(0, 0, A0), sqc2(0, 0x20, A0)),
	GPR(A0) = 0x1000; w32(0x1000, fb(1.f)); w32(0x1004, fb(2.f)); w32(0x1008, fb(3.f)); w32(0x100C, fb(4.f));
	RUN();
	CHECK_EQ(fb(VF(1).f[3]), fb(4.0f));
	CHECK_EQ(r64(0x1010), r64(0x1000)); CHECK_EQ(r64(0x1018), r64(0x1008));
	CHECK_EQ(VF(0).uw[0], 0); CHECK_EQ(fb(VF(0).f[3]), fb(1.0f)); // vf0 constant
	CHECK_EQ(r32(0x102C), fb(1.0f));)

TESTCASE(vu_qmfc2_qmtc2, ASM(qmtc2(A0, 2), qmfc2(V0, 2, true), qmtc2(A0, 0, true)),
	set128(c->r[A0], 0x1111222233334444ull, 0x5555666677778888ull);
	RUN();
	CHECK_EQ(VF(2).ud[0], 0x1111222233334444ull); CHECK_EQ(VF(2).ud[1], 0x5555666677778888ull);
	CHECK_EQ(GPR(V0), 0x1111222233334444ull); CHECK_EQ(GPRHI(V0), 0x5555666677778888ull);
	CHECK_EQ(fb(VF(0).f[3]), fb(1.0f));
	CHECK_EQ(g_log.waits, 2);)

TESTCASE(vu_cfc2_ctc2, ASM(ctc2(A0, 1), cfc2(V0, 1), ctc2(A1, 21), ctc2(A2, 20), cfc2(V1, 20), ctc2(A0, 17), ctc2(A3, 28), cfc2(T0, 27),
                           ctc2(T1, 22), cfc2(T2, 22), ctc2(A0, 0)),
	GPR(A0) = 0x12345; GPR(A1) = fb(2.0f); GPR(A2) = 0xFFFFFFFF; GPR(A3) = 0x202; GPR(T1) = 0xBF800000u; g_log.vu_ctrl[27] = 0x77;
	RUN();
	CHECK_EQ(VI(1), 0x2345); CHECK_EQ(GPR(V0), 0x2345);
	CHECK_EQ(VI(21), fb(2.0f));
	CHECK_EQ(VI(20), 0x3FFFFFFFu); CHECK_EQ(GPR(V1), 0x7FFFFF);
	CHECK_EQ(VI(17), 0); // MAC read-only
	CHECK_EQ(g_log.vu_ctrl[28], 0x202); CHECK_EQ(GPR(T0), 0x77);
	CHECK_EQ(GPR(T2), 0xFFFFFFFFBF800000ull);
	CHECK_EQ(VI(0), 0);)

TESTCASE(vu_add_mask, ASM(v1(40, X | Z, 3, 1, 2)),
	setf(VF(1), 1, 2, 3, 4); setf(VF(2), 10, 20, 30, 40); setf(VF(3), 9, 9, 9, 9); VI(17) = 0xFFFF;
	RUN();
	CHECK_EQ(fb(VF(3).f[0]), fb(11.f)); CHECK_EQ(fb(VF(3).f[1]), fb(9.f)); CHECK_EQ(fb(VF(3).f[2]), fb(33.f)); CHECK_EQ(fb(VF(3).f[3]), fb(9.f));
	CHECK_EQ(VI(17), 0);) // MAC cleared for masked and non-flagged fields

TESTCASE(vu_add_flags, ASM(v1(40, XYZW, 6, 4, 5)),
	setf(VF(4), -1, 0, 0, 1); VF(4).uw[2] = 0x7F7FFFFFu; setf(VF(5), 0, 0, 0, -1); VF(5).uw[2] = 0x7F7FFFFFu;
	RUN();
	CHECK_EQ(VF(6).uw[0], fb(-1.f)); CHECK_EQ(VF(6).uw[1], 0); CHECK_EQ(VF(6).uw[2], 0x7F7FFFFFu); CHECK_EQ(VF(6).uw[3], 0);
	CHECK_EQ(VI(17), 0x2085); // x sign, y zero, z overflow, w zero
	CHECK_EQ(VI(16), 0x2CB);) // Z S O + sticky

TESTCASE(vu_bc_q_i, ASM(v1(1, XYZW, 3, 1, 2), v1(32, XYZW, 4, 1, 0), v1(34, XYZW, 5, 1, 0), v1(7, XYZW, 6, 1, 2), v1(36, XYZW, 7, 1, 0),
                        v1(38, XYZW, 8, 1, 0), v1(24, XYZW, 9, 1, 2), v1(28, XYZW, 10, 1, 0), v1(30, XYZW, 11, 1, 0)),
	setf(VF(1), 1, 2, 3, 4); setf(VF(2), 10, 20, 30, 40); VI(22) = fb(0.5f); VI(21) = fb(100.f);
	RUN();
	auto V = [&](int r, float x, float y, float z, float w) { CHECK_EQ(VF(r).uw[0], fb(x)); CHECK_EQ(VF(r).uw[1], fb(y)); CHECK_EQ(VF(r).uw[2], fb(z)); CHECK_EQ(VF(r).uw[3], fb(w)); };
	V(3, 21, 22, 23, 24);       // vaddy
	V(4, 1.5f, 2.5f, 3.5f, 4.5f); // vaddq
	V(5, 101, 102, 103, 104);   // vaddi
	V(6, -39, -38, -37, -36);   // vsubw
	V(7, 0.5f, 1.5f, 2.5f, 3.5f); // vsubq
	V(8, -99, -98, -97, -96);   // vsubi
	V(9, 10, 20, 30, 40);       // vmulx
	V(10, 0.5f, 1, 1.5f, 2);    // vmulq
	V(11, 100, 200, 300, 400);) // vmuli

TESTCASE(vu_full_ops, ASM(v1(44, XYZW, 3, 2, 1), v1(42, XYZW, 4, 1, 2), v1(43, XYZW, 5, 1, 6), v1(47, XYZW, 7, 1, 6)),
	setf(VF(1), 1, 2, 3, 4); setf(VF(2), 10, 20, 30, 40); setf(VF(6), -1, 5, -3, 4);
	RUN();
	CHECK_EQ(fb(VF(3).f[0]), fb(9.f)); CHECK_EQ(fb(VF(3).f[3]), fb(36.f));     // vsub
	CHECK_EQ(fb(VF(4).f[1]), fb(40.f)); CHECK_EQ(fb(VF(4).f[3]), fb(160.f));   // vmul
	CHECK_EQ(fb(VF(5).f[0]), fb(1.f)); CHECK_EQ(fb(VF(5).f[1]), fb(5.f)); CHECK_EQ(fb(VF(5).f[2]), fb(3.f)); CHECK_EQ(fb(VF(5).f[3]), fb(4.f)); // vmax
	CHECK_EQ(fb(VF(7).f[0]), fb(-1.f)); CHECK_EQ(fb(VF(7).f[1]), fb(2.f)); CHECK_EQ(fb(VF(7).f[2]), fb(-3.f)); CHECK_EQ(fb(VF(7).f[3]), fb(4.f));) // vmini

TESTCASE(vu_acc_ops, ASM(v2(42, XYZW, 1, 2), v1(41, XYZW, 3, 1, 1), v1(45, XYZW, 4, 1, 1), v1(8, XYZW, 5, 1, 2), v1(13, XYZW, 6, 1, 2),
                         v1(33, XYZW, 7, 1, 0), v1(35, XYZW, 8, 1, 0), v1(37, XYZW, 9, 1, 0), v1(39, XYZW, 10, 1, 0)),
	setf(VF(1), 1, 2, 3, 4); setf(VF(2), 10, 20, 30, 40); VI(22) = fb(2.f); VI(21) = fb(3.f);
	RUN(); // ACC = vf1*vf2 = (10,40,90,160)
	auto V = [&](int r, float x, float y, float z, float w) { CHECK_EQ(VF(r).uw[0], fb(x)); CHECK_EQ(VF(r).uw[1], fb(y)); CHECK_EQ(VF(r).uw[2], fb(z)); CHECK_EQ(VF(r).uw[3], fb(w)); };
	V(3, 11, 44, 99, 176);  // vmadd
	V(4, 9, 36, 81, 144);   // vmsub
	V(5, 20, 60, 120, 200); // vmaddx
	V(6, -10, 0, 30, 80);   // vmsuby
	V(7, 12, 44, 96, 168);  // vmaddq
	V(8, 13, 46, 99, 172);  // vmaddi
	V(9, 8, 36, 84, 152);   // vmsubq
	V(10, 7, 34, 81, 148);) // vmsubi

TESTCASE(vu_acc_dest_ops, ASM(v2(40, XYZW, 1, 2), v1(42, XYZW, 11, 0, 0), v2(44, XYZW, 1, 2), v2(41, XYZW, 1, 1),
                              v2(0, XYZW, 1, 2), v2(5, XYZW, 1, 2), v2(8, XYZW, 1, 2), v2(15, XYZW, 1, 2), v2(24, XYZW, 1, 2),
                              v2(28, XYZW, 1, 0), v2(30, XYZW, 1, 0), v2(32, XYZW, 1, 0), v2(34, XYZW, 1, 0), v2(33, XYZW, 1, 0),
                              v2(35, XYZW, 1, 0), v2(36, XYZW, 1, 0), v2(38, XYZW, 1, 0), v2(37, XYZW, 1, 0), v2(39, XYZW, 1, 0), v2(45, XYZW, 1, 1)),
	setf(VF(1), 1, 2, 3, 4); setf(VF(2), 10, 20, 30, 40); VI(22) = fb(2.f); VI(21) = fb(3.f);
	RUN();
	// vadda -> (11,22,33,44); vsuba -> (-9,-18,-27,-36); vmadda vf1*vf1 -> (-8,-14,-18,-20); vaddax -> (11,12,13,14);
	// vsubay -> (-19,-18,-17,-16); vmaddax -> (-9,2,13,24); vmsubaw -> (-49,-78,-107,-136); vmulax -> (10,20,30,40);
	// vmulaq (2,4,6,8); vmulai (3,6,9,12); vaddaq (3,4,5,6); vaddai (4,5,6,7); vmaddaq 4+2*v -> (6,9,12,15);
	// vmaddai +3v -> (9,15,21,27); vsubaq v-2 -> (-1,0,1,2); vsubai v-3 -> (-2,-1,0,1); vmsubaq -2v -> (-4,-5,-6,-7);
	// vmsubai -3v -> (-7,-11,-15,-19); vmsuba -v*v -> (-8,-15,-24,-35)
	CHECK_EQ(fb(c->vacc.f[0]), fb(-8.f)); CHECK_EQ(fb(c->vacc.f[1]), fb(-15.f)); CHECK_EQ(fb(c->vacc.f[2]), fb(-24.f)); CHECK_EQ(fb(c->vacc.f[3]), fb(-35.f));)

TESTCASE(vu_acc_dest_steps, ASM(v2(40, XYZW, 1, 2), v1(41, XYZW, 3, 0, 0), v2(15, XYZW, 1, 2), v1(41, XYZW, 4, 0, 0), v2(37, XYZW, 1, 0), v1(41, XYZW, 5, 0, 0)),
	setf(VF(1), 1, 2, 3, 4); setf(VF(2), 10, 20, 30, 40); VI(22) = fb(2.f);
	RUN(); // vmadd vfN, vf0, vf0 = ACC + 0*0 (w: ACC + 1)
	CHECK_EQ(fb(VF(3).f[0]), fb(11.f)); CHECK_EQ(fb(VF(3).f[2]), fb(33.f)); CHECK_EQ(fb(VF(3).f[3]), fb(45.f));
	CHECK_EQ(fb(VF(4).f[0]), fb(-29.f)); CHECK_EQ(fb(VF(4).f[1]), fb(-58.f)); CHECK_EQ(fb(VF(4).f[3]), fb(-115.f)); // vmsubaw: ACC -= vf1*40
	CHECK_EQ(fb(VF(5).f[0]), fb(-31.f)); CHECK_EQ(fb(VF(5).f[3]), fb(-123.f));)                               // vmsubaq: ACC -= vf1*2

TESTCASE(vu_cross_product, ASM(v2(46, XYZ, 1, 2), v1(46, XYZ, 3, 2, 1)),
	setf(VF(1), 1, 2, 3, 0); setf(VF(2), 4, 5, 6, 0); VF(3).f[3] = 7;
	RUN();
	CHECK_EQ(fb(c->vacc.f[0]), fb(12.f)); CHECK_EQ(fb(c->vacc.f[1]), fb(12.f)); CHECK_EQ(fb(c->vacc.f[2]), fb(5.f));
	CHECK_EQ(fb(VF(3).f[0]), fb(-3.f)); CHECK_EQ(fb(VF(3).f[1]), fb(6.f)); CHECK_EQ(fb(VF(3).f[2]), fb(-3.f)); CHECK_EQ(fb(VF(3).f[3]), fb(7.f));)

TESTCASE(vu_minmax_bc, ASM(v1(17, XYZW, 3, 1, 2), v1(29, XYZW, 4, 1, 0), v1(20, XYZW, 5, 1, 2), v1(31, XYZW, 6, 1, 0)),
	setf(VF(1), 1, -2, 3, -4); setf(VF(2), 0, 0.5f, 0, 0); VI(21) = fb(-3.f);
	RUN();
	CHECK_EQ(fb(VF(3).f[0]), fb(1.f)); CHECK_EQ(fb(VF(3).f[1]), fb(0.5f)); CHECK_EQ(fb(VF(3).f[3]), fb(0.5f)); // vmaxy
	CHECK_EQ(fb(VF(4).f[1]), fb(-2.f)); CHECK_EQ(fb(VF(4).f[3]), fb(-3.f));                           // vmaxi
	CHECK_EQ(fb(VF(5).f[0]), fb(0.f)); CHECK_EQ(fb(VF(5).f[1]), fb(-2.f));                            // vminix
	CHECK_EQ(fb(VF(6).f[0]), fb(-3.f)); CHECK_EQ(fb(VF(6).f[3]), fb(-4.f));)                          // vminii

TESTCASE(vu_move_ops, ASM(v2(29, XYZW, 4, 3), v2(48, X | Y, 1, 5), v2(49, XYZW, 1, 6), v2(48, XYZW, 1, 0)),
	setf(VF(1), 1, 2, 3, 4); setf(VF(4), -1, -2, 3, 0); VF(4).uw[3] = 0x80000000u; setf(VF(5), 9, 9, 9, 9);
	RUN();
	CHECK_EQ(fb(VF(3).f[0]), fb(1.f)); CHECK_EQ(fb(VF(3).f[1]), fb(2.f)); CHECK_EQ(VF(3).uw[3], 0);          // vabs
	CHECK_EQ(fb(VF(5).f[0]), fb(1.f)); CHECK_EQ(fb(VF(5).f[1]), fb(2.f)); CHECK_EQ(fb(VF(5).f[2]), fb(9.f));          // vmove.xy
	CHECK_EQ(fb(VF(6).f[0]), fb(2.f)); CHECK_EQ(fb(VF(6).f[1]), fb(3.f)); CHECK_EQ(fb(VF(6).f[2]), fb(4.f)); CHECK_EQ(fb(VF(6).f[3]), fb(1.f)); // vmr32
	CHECK_EQ(fb(VF(0).f[3]), fb(1.f));)

TESTCASE(vu_ftoi_itof, ASM(v2(20, XYZW, 1, 2), v2(21, X, 1, 3), v2(22, X, 1, 4), v2(23, X, 1, 5),
                           v2(16, XYZW, 6, 7), v2(17, X, 8, 9), v2(18, X, 10, 11), v2(19, X, 12, 13)),
	setf(VF(1), 1.5f, -1.5f, 3e9f, -3e9f);
	setw(VF(6), 1, static_cast<uint32_t>(-1), 100, 0); setw(VF(8), 24, 0, 0, 0); setw(VF(10), 6144, 0, 0, 0); setw(VF(12), 49152, 0, 0, 0);
	RUN();
	CHECK_EQ(VF(2).uw[0], 1); CHECK_EQ(VF(2).uw[1], static_cast<uint32_t>(-1)); CHECK_EQ(VF(2).uw[2], 0x7FFFFFFFu); CHECK_EQ(VF(2).uw[3], 0x80000000u);
	CHECK_EQ(VF(3).uw[0], 24); CHECK_EQ(VF(4).uw[0], 6144); CHECK_EQ(VF(5).uw[0], 49152);
	CHECK_EQ(fb(VF(7).f[0]), fb(1.f)); CHECK_EQ(fb(VF(7).f[1]), fb(-1.f)); CHECK_EQ(fb(VF(7).f[2]), fb(100.f)); CHECK_EQ(fb(VF(7).f[3]), fb(0.f));
	CHECK_EQ(fb(VF(9).f[0]), fb(1.5f)); CHECK_EQ(fb(VF(11).f[0]), fb(1.5f)); CHECK_EQ(fb(VF(13).f[0]), fb(1.5f));)

TESTCASE(vu_clip, ASM(v2(31, XYZ, 1, 2)),
	setf(VF(1), 2, -2, 0.5f, 0); setf(VF(2), 0, 0, 0, -1.0f); VI(18) = 0x3;
	RUN();
	CHECK_EQ(VI(18), 0xC9);) // (old << 6) | +x | -y

TESTCASE(vu_div_sqrt, ASM(v2(56, (3 << 2) | 1, 2, 1), v1(28, X, 3, 4, 0), v2(56, (0 << 2) | 0, 1, 0), v1(28, X, 5, 4, 0),
                          v2(57, (0 << 2), 0, 7), v1(28, X, 6, 4, 0), v2(57, (1 << 2), 0, 7), v1(28, X, 8, 4, 0), v2(58, (0 << 2) | 1, 1, 7)),
	setf(VF(1), 1, 2, 3, 4); setf(VF(2), 10, 20, 30, 40); setf(VF(4), 1, 0, 0, 0); setf(VF(7), 16, -16, 0, 0);
	RUN();
	CHECK_EQ(fb(VF(3).f[0]), fb(5.f));              // vdiv Q = vf2.y / vf1.w
	CHECK_EQ(VF(5).uw[0], 0x7F7FFFFFu);     // vdiv by 0 -> FMAX
	CHECK_EQ(fb(VF(6).f[0]), fb(4.f));              // vsqrt
	CHECK_EQ(fb(VF(8).f[0]), fb(4.f));              // vsqrt of negative uses |x|
	CHECK_EQ(VI(22), fb(0.5f));             // vrsqrt Q = vf1.y / sqrt(vf7.x)
	CHECK_EQ(VI(16) & 0xC00u, 0xC00u);)     // sticky DS (div by 0) and IS (negative sqrt)

TESTCASE(vu_div_zero_zero, ASM(v2(56, 0, 0, 0)),
	RUN();
	CHECK_EQ(VI(22), 0x7F7FFFFFu);
	CHECK_EQ(VI(16) & 0x30u, 0x10u);) // 0/0: I

TESTCASE(vu_integer, ASM(v1(48, 0, 3, 1, 2), v1(49, 0, 4, 1, 2), v1(52, 0, 5, 1, 2), v1(53, 0, 6, 1, 2), v1(50, 0, 29, 1, 7),
                         v1(50, 0, 15, 1, 8), v1(49, 0, 9, 2, 1), v1(48, 0, 0, 1, 2)),
	VI(1) = 5; VI(2) = 3;
	RUN();
	CHECK_EQ(VI(3), 8); CHECK_EQ(VI(4), 2); CHECK_EQ(VI(5), 1); CHECK_EQ(VI(6), 7);
	CHECK_EQ(VI(7), 2); CHECK_EQ(VI(8), 20); CHECK_EQ(VI(9), 0xFFFE); CHECK_EQ(VI(0), 0);)

TESTCASE(vu_mtir_mfir, ASM(v2(60, 1, 1, 3), v2(61, XYZW, 2, 4)),
	VF(1).uw[1] = 0x12345678; VI(2) = 0xFFFE;
	RUN();
	CHECK_EQ(VI(3), 0x5678);
	for (int k = 0; k < 4; k++) CHECK_EQ(VF(4).uw[k], 0xFFFFFFFEu);)

TESTCASE(vu_ilwr_iswr, ASM(v2(62, Y, 1, 3), v2(63, Z, 1, 3)),
	VI(1) = 2; uint32_t v = 0x1234ABCD; std::memcpy(g_vu0 + 36, &v, 4);
	RUN();
	CHECK_EQ(VI(3), 0xABCD);
	uint32_t s; std::memcpy(&s, g_vu0 + 40, 4);
	CHECK_EQ(s, 0xABCD);)

TESTCASE(vu_lqi_sqi, ASM(v2(52, XYZW, 1, 1), v2(53, XYZW, 1, 2), v2(54, X, 1, 2), v2(55, W, 1, 2)),
	VI(1) = 3; VI(2) = 5; const float m[4] = {1, 2, 3, 4}; std::memcpy(g_vu0 + 48, m, 16); setf(VF(2), 9, 9, 9, 9);
	RUN();
	CHECK_EQ(fb(VF(1).f[0]), fb(1.f)); CHECK_EQ(fb(VF(1).f[3]), fb(4.f)); // vlqi
	float o[4]; std::memcpy(o, g_vu0 + 80, 16);
	CHECK_EQ(fb(o[0]), fb(1.f)); CHECK_EQ(fb(o[3]), fb(4.f)); // vsqi
	CHECK_EQ(VI(1), 3);                     // vlqi ++ then vlqd --
	CHECK_EQ(fb(VF(2).f[0]), fb(1.f)); CHECK_EQ(fb(VF(2).f[1]), fb(9.f));
	CHECK_EQ(VI(2), 5);)

TESTCASE(vu_random, ASM(v2(66, 0, 1, 0), v2(65, XYZW, 0, 2), v2(64, X, 0, 3), v2(67, 1, 1, 0)),
	VF(1).uw[0] = 0x12345678; VF(1).uw[1] = 0x00FFFFFF;
	RUN();
	CHECK_EQ(VF(2).uw[0], 0x3FB45678u); CHECK_EQ(VF(2).uw[3], 0x3FB45678u);
	CHECK_EQ(VF(3).uw[0], 0x3FE8ACF1u);
	CHECK_EQ(VI(20), 0x3F97530Eu);)

TESTCASE(vu_callms, ASM(vcallms(0x10), vcallmsr(), v2(47, 0, 0, 0), v2(59, 0, 0, 0)),
	g_log.vu_ctrl[27] = 0x20;
	RUN();
	CHECK_EQ(g_log.callms, 2);
	CHECK_EQ(g_log.last_addr, 0x100);)

TESTCASE(vu_vf0_write_discarded, ASM(v1(40, XYZW, 0, 1, 2)),
	setf(VF(1), 1, 2, 3, 4); setf(VF(2), 1, 1, 1, 1);
	RUN();
	CHECK_EQ(VF(0).uw[0], 0); CHECK_EQ(fb(VF(0).f[3]), fb(1.f));)
