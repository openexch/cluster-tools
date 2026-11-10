# Java bridge parity: the archive side (ChainResumeMemoTest)

The C++ settlement bridge replaces the Java bridge in openexch/core
(`core-bridge/src/test/java/com/openexchange/core/bridge/`, read at `money_schema_ref` in
`dependencies.lock`). Every Java test has a row in this file or in
[`domain/bridge/tests/PARITY.md`](../../domain/bridge/tests/PARITY.md), with one verdict:

- CARRIED: the same rule, the C++ test named.
- CHANGED: the same intent under a different rule, the reason given.
- RETIRED: the rule no longer exists in the C++ bridge, the reason given.

This file holds `ChainResumeMemoTest`, the per-source resume memo. The other Java tests are in the domain
file. The check is `app/bridge/it/parity-check.sh` (ctest `bridge.parity`, registered when
`CT_PARITY_CORE_TREE` points at a core checkout): it fails when a Java `@Test` method name appears in
neither file.

C++ names: `archive/tests/*Test.cpp` (pure, `ct_archive_tests`); "IT" means a scenario of
`archive/it/ArchiveIntegration.cpp` (`ct_archive_it`, run by `archive/it/run-it.sh`).

Two design changes explain most CHANGED and RETIRED rows:

- One cursor. The core's `lastAppliedJournalSeq` is the only cursor; a frame proves a resume position only
  when its `journalSeq` is at or below that cursor. The Java bridge kept two watermarks and sealed byte
  prefixes before a forward.
- One memo per source. Each journal archive has its own memo; a source switch invalidates nothing and a
  position is never reused on another source.

## ChainResumeMemoTest (27)

| # | Java case | Verdict | C++ |
|---|---|---|---|
| 1 | walksTheWholeChainWhenNothingIsRemembered | CARRIED | ResumeMemoTest.coldWalkFromChainStart |
| 2 | resumesPastTheDrainedPrefixAtTheSameSyncPoint | CARRIED (a proof at a stopped recording's stop starts the next recording) | ResumeMemoTest.prefixResumePastStoppedProvenRecordings |
| 3 | retainsProvenPrefixWhenBothWatermarksAdvance | CARRIED (one cursor) | ResumeMemoTest.retainedAcrossAReportAdvance |
| 4 | keepsThePrefixProvenBeforeAForwardButStopsExtending | CHANGED: no seal; a frame above the core cursor never proves | ResumeMemoTest.proofOnlyFromSeqAtOrBelowCursor |
| 5 | doesNotRememberARecordingDrainedAfterAForward | CHANGED: as 4 | ResumeMemoTest.proofOnlyFromSeqAtOrBelowCursor |
| 6 | doesNotRememberAGapInTheDrainedPrefix | CHANGED: the resume is at the last recording with a proof; a record of an earlier recording above the proof is re-journaled later or shows up as a hole, and a hole after a memo resume is verified from the chain start | ResumeMemoTest.lastRecordingWithAProofWins, ResumeMemoTest.holeAfterMemoResumeIsVerifiedFromChainStart, ChainWalkPropertyTest |
| 7 | forgetsEverythingWhenTheArchiveWasWipedAndRecordingIdsRestarted | CARRIED (a stopped recording's stop changed: history change) | ResumeMemoTest.historyChange |
| 8 | forgetsEverythingWhenTheDrainedPrefixWasPurgedFromTheChain | CHANGED: retention of proven head recordings trims their proofs only; later proofs survive | ResumeMemoTest.headRetentionTrimsProvenRecordings |
| 9 | doesNotResumePastAChainThatShrankBelowTheRememberedPrefix | CARRIED (a tail recording gone is a history change) | ResumeMemoTest.middleRecordingGoneIsHistoryChange |
| 10 | keepsResumingAcrossRepeatedFailuresAtTheSameSyncPoint | CARRIED | ResumeMemoTest.repeatedFailuresKeepResumingForward |
| 11 | replayStartsAtTheRecordingStartWhenNoHeadHasBeenSkipped | CARRIED | ResumeMemoTest.highWaterOnlyAdvances |
| 12 | replayResumesPastTheSkippedHead | CARRIED | ResumeMemoTest.highWaterOnlyAdvances |
| 13 | theSkippedHeadHighWaterOnlyEverAdvances | CARRIED | ResumeMemoTest.highWaterOnlyAdvances |
| 14 | theSkippedHeadSurvivesASyncPointMoveForARecordingStillInTheChain | CARRIED | ResumeMemoTest.retainedAcrossAReportAdvance |
| 15 | theSkippedHeadIsForgottenWhenTheRecordingLeavesTheChain | CARRIED (as a head trim) | ResumeMemoTest.headRetentionTrimsProvenRecordings |
| 16 | aWipedRecordingWithTheSameIdDoesNotReuseAStaleSkippedHead | CARRIED | ResumeMemoTest.historyChange |
| 17 | differentSourceWithIdenticalDescriptorsInvalidatesBothAndNeverResurrectsA | RETIRED: one memo per source; a source switch invalidates nothing and a position is never moved to another source | ResumeMemoTest.perSourceMemoSurvivesFailBack, ResumeMemoTest.ringProofLandsOnItsOwnSourceOnly, IT perSourceMemoSurvivesFailBack |
| 18 | reusedIdAndIdenticalExtentsWithChangedIncarnationInvalidatesBoth | CARRIED (three incarnation fields; old descriptors never restore) | ResumeMemoTest.historyChange |
| 19 | eitherWatermarkRegressingDropsBothEvenIfTheOtherAdvances | CHANGED: one cursor; a cursor below the highest proof, or another `meEpoch`, invalidates every source | ResumeMemoTest.cursorRegressionInvalidatesEverySource, ResumeMemoTest.epochChangeInvalidatesEverySource |
| 20 | emptyChainOnAnotherSourceAlsoInvalidatesBoth | RETIRED: per-source memo (another source's listing never touches this one) | ResumeMemoTest.perSourceMemoSurvivesFailBack |
| 21 | legacyRecordingWithoutIncarnationCannotEnableMemo | RETIRED: the Aeron C client's listRecordingsForUri always carries the incarnation; there is no three-field recording | (none) |
| 22 | activeToStoppedPreservesByteMarkButNeverTreatsActiveFrontierAsDrained | CARRIED | ResumeMemoTest.activeToStoppedPreservesTheMark |
| 23 | stopBeforeRememberedActiveFrontierInvalidatesBoth | CARRIED | ResumeMemoTest.stopBeforeTheRememberedFrontierIsHistoryChange |
| 24 | emptyThenFullySkippedStoppedChainCanBeExhaustedAndGainSuccessor | CARRIED | ResumeMemoTest.emptyThenExhaustedChainGainsSuccessor |
| 25 | forwardSealsBytePrefixBeforeUnacknowledgedDuplicateCanSkip | CHANGED: an unacknowledged forward is above the cursor and never proves | ResumeMemoTest.proofOnlyFromSeqAtOrBelowCursor |
| 26 | terminalOnlyAndTradeOnlyWatermarkProgressKeepBothProofs | RETIRED: one dense `journalSeq` cursor for every record kind | ResumeMemoTest.retainedAcrossAReportAdvance |
| 27 | malformedBoundsRemainVisibleEvenIfTheRecordingLooksEmpty | CARRIED (a source fault) | ChainModelTest.malformedStoppedExtentIsNotSilentlySkipped |
