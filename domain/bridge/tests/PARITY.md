# Java bridge parity: domain, translation and process wiring

The C++ settlement bridge replaces the Java bridge in openexch/core
(`core-bridge/src/test/java/com/openexchange/core/bridge/`, read at `money_schema_ref` in
`dependencies.lock`). Every Java test maps to a C++ test that carries its intent, or is retired with the
reason. `ChainResumeMemoTest` (the archive resume memo) is in
[`archive/tests/PARITY.md`](../../../archive/tests/PARITY.md); every other Java test class is here.

The check is `app/bridge/it/parity-check.sh` (ctest `bridge.parity`, registered when `CT_PARITY_CORE_TREE`
points at a core checkout): it fails when a Java `@Test` method name appears in neither file.

Names: C++ tests are doctest cases under `domain/bridge/tests`, `archive/tests` and `app/bridge/tests`
(`.name` repeats the class of the previous entry). A drill is a scenario of `app/bridge/it/run-bridge-it.sh`
on real processes. `bridge.e3_app` is a ctest name.

Rules that replace Java behaviour, referenced in the notes:

- The cursor is the core's `lastAppliedJournalSeq`, dense over every journal record. A record at
  `seq <= cursor` is skipped; nothing is ever forwarded past a hole.
- A resume proof comes only from a record at or below the cursor, on its own source and recording. Staged,
  unsent or unacknowledged records never prove.
- Money schema v7 templates replace the v5 shapes: trades go as one 62 batch (cap 64), terminals as a 63
  carrying the raw status. The v5 SettleBatch and TerminalRelease shapes are refused for the BRIDGE role.

## BridgeFilterTest (6)

| Java test | C++ | Note |
|---|---|---|
| `startupSkipsBelowWatermarkAndRedeliversTheBoundaryGroupInclusively` | `CursorTableTest.skipBelowLastNeverOffers`, `.bridgeRestart3xSendsNothingBelowCursor`, `.terminalBelowCursorNeverForwarded` | the inclusive boundary is retired: a record at `seq <= C` is a skip |
| `denseTradeSequenceForwardsAndDuplicatesSkipEvenAfterProgress` | `CursorTableTest.skipBelowLastNeverOffers`, `BridgeDomainPropertyTest` | the dense key is `journalSeq`, not `tradeId` |
| `tradeGapHaltsAndLatchesForever` | `RotationTest.holeOnEverySourceHaltsGap`, `HaltMachineTest.gapHaltRecoversWhenTheCursorPassesSeen` | the dense `tradeId` halt is retired (the core enforces density); a `journalSeq` hole on every source halts GAP, and the halt resumes by itself once the core cursor passes `seen - 1` |
| `haltOnGapFalseForwardsPastTheGapButCountsIt` | retired | `haltOnGap=false` is gone (`BRIDGE_HALT_ON_GAP` is refused): nothing is forwarded past a hole, `CursorTableTest.holeIsNeverForwarded` |
| `retentionRaceSurfacesAsAGapNotASilentSkip` | `RotationTest.holeOnOneSourceFailsOver`, `.holeFromAMemoReadIsVerifiedFirst`, `ChainWalkPropertyTest` | a purge race is a hole on that source: fail over first; a hole read after a memo resume is verified without the memo before it can become a GAP |
| `freshLedgerEpochForwardsFromTradeOne` | `CursorTableTest.unboundSyncAdoptsTheEpochOfSeqOne`, `.unboundSeqOneMustBeEngineConfig`, `StormGuardTest.unboundSilentCoreKeepsRetryingAndBindsOnceV3` | an unbound sync starts at seq 1, which must be a 68 |

## JournalToMoneyTranslatorTest (12)

| Java test | C++ | Note |
|---|---|---|
| `translatesTradeFieldForField_takerIsBuyTrue` | `TranslatorGoldenTest` (`fx_trade`, the Java reference translation), `TranslatorDifferentialTest`, `BridgeAppCodecTest.tradeGetterReadsTheGeneratedOffsets` | every field verbatim against the Java reference bytes |
| `translatesTradeFieldForField_takerIsBuyFalse` | `TranslatorGoldenTest`, `RawEnumsAreWrittenVerbatim` (takerIsBuy 0..255 raw) | |
| `translateTradeCarriesLargeEgressSeqAsJournalPositionVerbatim` | `TranslatorGoldenTest` (`head_extremes`), `TranslatorDifferentialTest`, `TranslatorIdentityTest` | money v7 carries `egressSeq` and `journalSeq` as separate fields |
| `stagedTradesEncodeAsOneSettleBatchFieldForFieldInOrder` | `TranslatorGoldenTest` (`two_trades`, `trades_64`), `StagingTest.outcomeIsForwardedAndFlushesStagedTrades` | the batch is the 62 |
| `stagingPastTheCapThrowsRatherThanDroppingASettlement` | `StagingTest.capSixtyFourSplits`, `.backPressureKeepsEverythingAndAborts`, `TranslatorGoldenTest` (`trades_65`) | the cap flushes at the 64th; nothing staged is ever dropped |
| `translatesTerminalFilledToTerminalReleaseShape` | retired | the v5 TerminalRelease is refused for BRIDGE; the 63 carries the status raw: `TranslatorGoldenTest` (`fx_terminal`), `RawEnumsAreWrittenVerbatim` |
| `translatesTerminalCancelledToTerminalReleaseShape` | retired | as above |
| `translatesTerminalRejectedToTerminalReleaseShape` | retired | as above (`rejectReason` copied as a plain uint8) |
| `translateTerminalCarriesLargeEgressSeqAsJournalPositionVerbatim` | `TranslatorIdentityTest` (63 head), `TranslatorDifferentialTest` | |
| `isTradeAndIsTerminalRecognizeTheirOwnMessages` | `FramePeekTest`, `E1GateTest` | the template is read by the archive admission gate, not by the translator |
| `sniffersRejectMatchingTemplateIdOnTheWrongSchema` | `FramePeekTest`, `RotationTest.schema3RecordHaltsSourceSchema`, `E1GateTest` (`schema3_record`) | a schema 3 record is refused by the gate and marks the source bad (never skipped as in Java) |
| `sniffersRejectUnrelatedTemplateIdOnTheRightSchema` | `FramePeekTest`, `BridgeCodecTest.aRecordOffItsTemplateShapeIsNotEncoded` | |

## BridgeAgentResumeSafetyTest (4)

| Java test | C++ | Note |
|---|---|---|
| `duplicateOfUnsentTradeDoesNotTurnStagingIntoAnAppliedByteCheckpoint` | `RingTest.syncStartKeepsProofsBelowCursorDropsTheRest`, `.keyTableSurvivesStagingAcrossAPop` | a proof comes only from a popped ring entry (`seq <= C'`) or a skip below the memo cursor; staging never proves |
| `failedInclusiveTerminalOfferSealsMemoBeforeALaterSkip` | `StagingTest.backPressureKeepsEverythingAndAborts`, `CursorTableTest.terminalBelowCursorNeverForwarded` | a back-pressured offer advances nothing; no inclusive terminal exists |
| `inclusiveTerminalInEarlierRecordingDoesNotBlockLaterAppliedByteResume` | `RingTest.memoProvesAllKinds`, `.ringProofLandsOnItsOwnSourceOnly` | every kind proves its run on its own source and recording |
| `duplicateOfEarlierUnsentTradeCannotStartALaterRecordingByteProof` | `RingTest.memoProvesAllKinds`, `ResumeMemoTest` | a proof is valid only for `seq <= C`, `meEpoch == E` and a frame end inside the listed recording (`ResumeMemo::prove`) |

## BridgeAgentEmptyRecordingTest (4)

| Java test | C++ | Note |
|---|---|---|
| `stoppedEmptyRecordingIsSkipped` | `ChainModelTest.stoppedEmptyRecordingIsSkipped`, drill `rk5` | on a real archive |
| `stoppedRecordingWithBytesIsFollowed` | `ChainModelTest.stoppedRecordingWithBytesIsFollowed`, drill `rk5` | |
| `malformedStoppedExtentIsNotSilentlySkipped` | `ChainModelTest.malformedStoppedExtentIsNotSilentlySkipped` | a MALFORMED_EXTENT fault, counted by `ObserverTap` (`BridgeAgentTest.everyFaultKindIsCounted`) |
| `activeRecordingIsNeverSkipped` | `ChainModelTest.activeRecordingIsNeverSkipped` | |

## BridgeEndToEndTest (9)

| Java test | C++ | Note |
|---|---|---|
| `byteResumeSurvivesAToBToAWithDifferentRealFrameBoundaries` | drill `rk8` | per-source memo, no byte position reused across sources |
| `emptyHeadStoppedEofAndActiveFrontierContinueIntoSuccessor` | drills `rk5`, `rk3`, `FollowPolicyTest.recordingStopsThenSuccessor` | |
| `earlierInclusiveTerminalDoesNotForceRescanningLaterAppliedRecording` | retired | the v5 inclusive terminal boundary is gone; proofs are per recording: `RingTest.memoProvesAllKinds`, the cold-restart round of drill `rk1` |
| `backlogBatchesApplyInOrderAndSessionSurvivesIdlePastTimeout` | drill `rk6` | cap-64 62 frames on the real `mtu=8192` ingress, then 6 s idle |
| `stoppedReplayEndingBeforeCatalogEofIsAVisibleFailure` | `FollowPolicyTest.stoppedReplayBeforeEofIsFailure` | |
| `rejectedRememberedOffsetClearsBothProofsAndNextReplayStartsSafely` | `ResumeMemoTest.rejectedRememberedPositionClearsProofs` | |
| `replayCapacityRejectionPreservesValidByteAndDrainedPrefixProofs` | `ResumeMemoTest.capacityRejectionKeepsProofs` | |
| `bridgeSettlesReleasesAndResumesAcrossARestart` | drills `rk0-external`, `rk0-embedded`, `rk1`, `bridge.e3_app` (restarts over the same session) | |
| `bridgeRecoversWhenJournalSourceDiesMidLiveFollow` | drill `rk3` | the journal source dies during live follow |

## BridgeMetricsServerTest (3) and LatencyHistogramTest (3)

| Java test | C++ | Note |
|---|---|---|
| `metricsRendersAllSeriesWithCurrentValues` | `MetricsRenderTest.prometheusGolden`, `.everyAlertedSeriesIsExported` | |
| `metricsRendersLatencyHistogramsReplayGaugesAndAnomalyCounters` | `MetricsRenderTest.prometheusGolden`, `CodecAppTest.forwardLatencyAndAckLatency` | buckets in ms (1 ms to 60 s), not the Java seconds labels |
| `healthReports200WhenHealthyAnd503WhenHalted` | `MetricsRenderTest.healthGolden`, `.haltedFollowsTheHaltRecord`, `MainTest.stalledHealthIs503BeforeTheExit` | 503 also on STALLED |
| `recordsKnownValuesIntoTheRightBuckets` | `AckMapTest.forwardLatencyAnomalyBounds`, `MetricsRenderTest.prometheusGolden` | |
| `bucketUpperBoundsAreInclusive` | `MetricsRenderTest.prometheusGolden` (3 ms in `le="5"`, 70 ms in `le="100"`) | |
| `totalOfAllBucketsEqualsCount` | `MetricsRenderTest.prometheusGolden` (`+Inf` = `_count`) | |

## AeDumpTest (4)

The Java `dump` tool is not part of ct-bridge and is not ported here.

| Java test | C++ | Note |
|---|---|---|
| `collectsBalancesHoldsAndPositionIntoTheExpectedJson` | not ported | not a bridge function |
| `collectsBatchFormsIntoTheSameJsonAsSingles` | not ported | |
| `dedupsRepeatedBalanceForSameUserAssetLastWriteWins` | not ported | |
| `terminatorsWithForeignCorrelationIdsDoNotComplete` | not ported | |
