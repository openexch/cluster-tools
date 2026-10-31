# Bridge parity (E4, domain half; RC2 S9b 13.18, cpp 3.2)

Every test of the frozen Java bridge (`assets-rc2corewt` b05b8e8, `assets-bridge/src/test/java/com/openexchange/assets/bridge/`)
maps to a C++ test that carries its intent, or is retired with the reason. S9c completes the table for the parts that
are process wiring (it fails if a Java test has no row). Counts as found at b05b8e8: `BridgeFilterTest` 6,
`JournalToMoneyTranslatorTest` 10 (the contract's "12" counted two removed cases; S9b deviation 3),
`BridgeAgentResumeSafetyTest` 4.

## BridgeFilterTest (6)

| Java test | C++ | Note |
|---|---|---|
| `startupSkipsBelowWatermarkAndRedeliversTheBoundaryGroupInclusively` | `CursorTableTest.skipBelowLastNeverOffers`, `.bridgeRestart3xSendsNothingBelowCursor`, `.terminalBelowCursorNeverForwarded` | the inclusive boundary is retired (cpp F3): a record at `seq <= C` is a SKIP, the RED is `TERMINAL_INCLUSIVE` |
| `denseTradeSequenceForwardsAndDuplicatesSkipEvenAfterProgress` | `CursorTableTest.skipBelowLastNeverOffers`, `BridgeDomainPropertyTest` (a) | the dense key is journalSeq, not tradeId |
| `tradeGapHaltsAndLatchesForever` | `RotationTest.holeOnEverySourceHaltsGap`, `HaltMachineTest.gapHaltRecoversWhenTheCursorPassesSeen` | the dense tradeId HALT is retired (cpp F2: the core's second invariant); a journalSeq hole halts GAP after the rotation, and the halt is latched but auto-resumes when the core passes `seen - 1` (cpp Q3, 9.4) |
| `haltOnGapFalseForwardsPastTheGapButCountsIt` | retired | `haltOnGap=false` is retired (cpp F5): nothing is ever forwarded past a hole (G1); `CursorTableTest.holeIsNeverForwarded` |
| `retentionRaceSurfacesAsAGapNotASilentSkip` | `RotationTest.holeOnOneSourceFailsOver`, `.holeFromAMemoReadIsVerifiedFirst`, S9a `ChainWalkPropertyTest` | a purge race is a hole on that source: fail over first; a memo-read hole is verified memo-free before it can become a GAP |
| `freshLedgerEpochForwardsFromTradeOne` | `CursorTableTest.unboundSyncAdoptsTheEpochOfSeqOne`, `.unboundSeqOneMustBeEngineConfig`, `StormGuardTest.unboundSilentCoreKeepsRetryingAndBindsOnceV3` | an unbound sync starts at seq 1, which must be a 68 (D-S9b-2) |

## JournalToMoneyTranslatorTest (10)

| Java test | C++ | Note |
|---|---|---|
| `translateTradeCarriesLargeEgressSeqAsJournalPositionVerbatim` | `TranslatorGoldenTest` (`head_extremes`), `TranslatorDifferentialTest`, `TranslatorIdentityTest` | money v7 carries egressSeq as `egressSeq` (61 @0), journalSeq as `journalSeq`; extremes from S9h's `random` |
| `stagedTradesEncodeAsOneSettleBatchFieldForFieldInOrder` | `TranslatorGoldenTest` (`two_trades`, `trades_64`), `StagingTest.outcomeIsForwardedAndFlushesStagedTrades` | the batch is the 62 (v5 SettleBatch 33 is refused for BRIDGE under behaviour 3, `DX:433-440`) |
| `stagingPastTheCapThrowsRatherThanDroppingASettlement` | `StagingTest.capSixtyFourSplits`, `.backPressureKeepsEverythingAndAborts`, `TranslatorGoldenTest` (`trades_65`) | the cap flushes at the 64th; nothing staged is ever dropped (G5) |
| `translatesTerminalFilledToTerminalReleaseShape` | retired | v5 TerminalRelease 6 is refused for BRIDGE under behaviour 3 (cpp T3); the 63 carries the status raw: `TranslatorGoldenTest` (`fx_terminal`), `RawEnumsAreWrittenVerbatim` |
| `translatesTerminalCancelledToTerminalReleaseShape` | retired | as above |
| `translatesTerminalRejectedToTerminalReleaseShape` | retired | as above (`rejectReason` copied as a plain uint8, FIX-14) |
| `translateTerminalCarriesLargeEgressSeqAsJournalPositionVerbatim` | `TranslatorIdentityTest` (63 head), `TranslatorDifferentialTest` | |
| `isTradeAndIsTerminalRecognizeTheirOwnMessages` | S9a `FramePeekTest`, `E1GateTest` | the template is read by the S9a gate (peek), not by the translator |
| `sniffersRejectMatchingTemplateIdOnTheWrongSchema` | S9a `FramePeekTest`, `RotationTest.schema3RecordHaltsSourceSchema`, `E1GateTest` (`schema3_record`) | a schema 3 record is refused by the gate and becomes BAD (never skipped like Java T5) |
| `sniffersRejectUnrelatedTemplateIdOnTheRightSchema` | S9a `FramePeekTest`, `BridgeCodecTest.aRecordOffItsTemplateShapeIsNotEncoded` | |

## BridgeAgentResumeSafetyTest (4)

| Java test | C++ | Note |
|---|---|---|
| `duplicateOfUnsentTradeDoesNotTurnStagingIntoAnAppliedByteCheckpoint` | `RingTest.syncStartKeepsProofsBelowCursorDropsTheRest`, `.keyTableSurvivesStagingAcrossAPop` | a proof comes only from a popped ring entry (`seq <= C'`) or a skip below the memo cursor; staging never proves |
| `failedInclusiveTerminalOfferSealsMemoBeforeALaterSkip` | `StagingTest.backPressureKeepsEverythingAndAborts`, `CursorTableTest.terminalBelowCursorNeverForwarded` | a back-pressured offer advances nothing; no inclusive terminal exists |
| `inclusiveTerminalInEarlierRecordingDoesNotBlockLaterAppliedByteResume` | `RingTest.memoProvesAllKinds`, `.ringProofLandsOnItsOwnSourceOnly` | every kind proves its run on its own source and recording |
| `duplicateOfEarlierUnsentTradeCannotStartALaterRecordingByteProof` | `RingTest.memoProvesAllKinds`, S9a `ResumeMemoTest` | a proof is valid only for `seq <= C`, `meEpoch == E`, a frame end inside the listed recording (`ResumeMemo::prove`) |
