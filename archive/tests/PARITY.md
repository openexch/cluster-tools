# Java bridge tests -> C++ archive tests (RC2 S9a contract 9.1, rc2-bridge-cpp 3.2 E4)

Read against assets `assets-rc2corewt` b05b8e8 (`assets-bridge/src/test/java/com/openexchange/assets/bridge/`).
Every Java case of the archive side has a row: CARRIED (same rule, C++ test named), CHANGED (same intent,
a different rule, reason given) or RETIRED (the rule is gone, reason given). Translation, filter and
core-session cases are S9b/S9c's and are listed at the end only so the table is complete for this
directory. C++ test names: `archive/tests/*Test.cpp` (pure, `ct_archive_tests`) and
`archive/it/ArchiveIntegration.cpp` (9.3, `ct_archive_it`).

## ChainResumeMemoTest (27)

| # | Java case | Verdict | C++ |
|---|---|---|---|
| 1 | walksTheWholeChainWhenNothingIsRemembered | CARRIED | ResumeMemoTest.coldWalkFromChainStart |
| 2 | resumesPastTheDrainedPrefixAtTheSameSyncPoint | CARRIED (a proof at a stopped recording's stop starts the next recording) | ResumeMemoTest.prefixResumePastStoppedProvenRecordings |
| 3 | retainsProvenPrefixWhenBothWatermarksAdvance | CARRIED (one cursor, `lastAppliedJournalSeq`) | ResumeMemoTest.retainedAcrossAReportAdvance |
| 4 | keepsThePrefixProvenBeforeAForwardButStopsExtending | CHANGED: no seal; a frame above the core cursor never proves (cpp M2) | ResumeMemoTest.proofOnlyFromSeqAtOrBelowCursor |
| 5 | doesNotRememberARecordingDrainedAfterAForward | CHANGED: as 4 | ResumeMemoTest.proofOnlyFromSeqAtOrBelowCursor |
| 6 | doesNotRememberAGapInTheDrainedPrefix | CHANGED: the resume is at the LAST recording with a proof (5.2 rule 3); a record of an earlier recording above the proof is re-journaled later or shows up as a hole, and a hole after a memo resume is verified from the chain start (5.2 rule 5) | ResumeMemoTest.lastRecordingWithAProofWins, ResumeMemoTest.holeAfterMemoResumeIsVerifiedFromChainStart, ChainWalkPropertyTest (K8 shape) |
| 7 | forgetsEverythingWhenTheArchiveWasWipedAndRecordingIdsRestarted | CARRIED (5.3 row 3: a stopped recording's stop changed) | ResumeMemoTest.historyChange |
| 8 | forgetsEverythingWhenTheDrainedPrefixWasPurgedFromTheChain | CHANGED: retention of proven head recordings trims their proofs only (5.3 row 4); later proofs survive | ResumeMemoTest.headRetentionTrimsProvenRecordings |
| 9 | doesNotResumePastAChainThatShrankBelowTheRememberedPrefix | CARRIED (a tail recording gone is a history change) | ResumeMemoTest.middleRecordingGoneIsHistoryChange |
| 10 | keepsResumingAcrossRepeatedFailuresAtTheSameSyncPoint | CARRIED | ResumeMemoTest.repeatedFailuresKeepResumingForward |
| 11 | replayStartsAtTheRecordingStartWhenNoHeadHasBeenSkipped | CARRIED | ResumeMemoTest.highWaterOnlyAdvances |
| 12 | replayResumesPastTheSkippedHead | CARRIED | ResumeMemoTest.highWaterOnlyAdvances |
| 13 | theSkippedHeadHighWaterOnlyEverAdvances | CARRIED | ResumeMemoTest.highWaterOnlyAdvances |
| 14 | theSkippedHeadSurvivesASyncPointMoveForARecordingStillInTheChain | CARRIED | ResumeMemoTest.retainedAcrossAReportAdvance |
| 15 | theSkippedHeadIsForgottenWhenTheRecordingLeavesTheChain | CARRIED (as a head trim) | ResumeMemoTest.headRetentionTrimsProvenRecordings |
| 16 | aWipedRecordingWithTheSameIdDoesNotReuseAStaleSkippedHead | CARRIED | ResumeMemoTest.historyChange |
| 17 | differentSourceWithIdenticalDescriptorsInvalidatesBothAndNeverResurrectsA | RETIRED: one memo per source (cpp M3); a source switch invalidates nothing (5.3 row 9) and a position is never moved to another source | ResumeMemoTest.perSourceMemoSurvivesFailBack, ResumeMemoTest.ringProofLandsOnItsOwnSourceOnly, IT perSourceMemoSurvivesFailBack |
| 18 | reusedIdAndIdenticalExtentsWithChangedIncarnationInvalidatesBoth | CARRIED (three incarnation fields, old descriptors never restore) | ResumeMemoTest.historyChange |
| 19 | eitherWatermarkRegressingDropsBothEvenIfTheOtherAdvances | CHANGED: one cursor; `C' < maxProven` (5.3 row 2) and another `meEpoch` (row 1) | ResumeMemoTest.cursorRegressionInvalidatesEverySource, ResumeMemoTest.epochChangeInvalidatesEverySource |
| 20 | emptyChainOnAnotherSourceAlsoInvalidatesBoth | RETIRED: per-source memo (another source's listing never touches this one) | ResumeMemoTest.perSourceMemoSurvivesFailBack |
| 21 | legacyRecordingWithoutIncarnationCannotEnableMemo | RETIRED: the C client's listRecordingsForUri always carries the incarnation; there is no three-field Recording | (none) |
| 22 | activeToStoppedPreservesByteMarkButNeverTreatsActiveFrontierAsDrained | CARRIED | ResumeMemoTest.activeToStoppedPreservesTheMark |
| 23 | stopBeforeRememberedActiveFrontierInvalidatesBoth | CARRIED | ResumeMemoTest.stopBeforeTheRememberedFrontierIsHistoryChange |
| 24 | emptyThenFullySkippedStoppedChainCanBeExhaustedAndGainSuccessor | CARRIED | ResumeMemoTest.emptyThenExhaustedChainGainsSuccessor |
| 25 | forwardSealsBytePrefixBeforeUnacknowledgedDuplicateCanSkip | CHANGED: an unacknowledged forward is above the cursor and never proves (cpp M2) | ResumeMemoTest.proofOnlyFromSeqAtOrBelowCursor |
| 26 | terminalOnlyAndTradeOnlyWatermarkProgressKeepBothProofs | RETIRED: one dense journalSeq cursor for every kind (cpp M2) | ResumeMemoTest.retainedAcrossAReportAdvance |
| 27 | malformedBoundsRemainVisibleEvenIfTheRecordingLooksEmpty | CARRIED (4.2 row 1, a source fault) | ChainModelTest.malformedStoppedExtentIsNotSilentlySkipped |

## BridgeAgentEmptyRecordingTest (4)

| Java case | Verdict | C++ |
|---|---|---|
| stoppedEmptyRecordingIsSkipped | CARRIED | ChainModelTest.stoppedEmptyRecordingIsSkipped, IT emptyGenesisRecordingThenData |
| stoppedRecordingWithBytesIsFollowed | CARRIED | ChainModelTest.stoppedRecordingWithBytesIsFollowed |
| malformedStoppedExtentIsNotSilentlySkipped | CARRIED | ChainModelTest.malformedStoppedExtentIsNotSilentlySkipped |
| activeRecordingIsNeverSkipped | CARRIED | ChainModelTest.activeRecordingIsNeverSkipped |

## BridgeAgentResumeSafetyTest (4)

All four guard the Java "seal before a forward" bookkeeping (staged or unsent trades and inclusive
terminals must not become a byte proof). RETIRED for the archive side: a proof is made only from a
frame whose seq is at or below the core's reported cursor (5.2 rule 1), so a staged, unsent or
unacknowledged record can never prove (ResumeMemoTest.proofOnlyFromSeqAtOrBelowCursor). The staging
and terminal rules themselves are S9b's (translation, 6.3).

| Java case | Verdict |
|---|---|
| duplicateOfUnsentTradeDoesNotTurnStagingIntoAnAppliedByteCheckpoint | RETIRED (5.2 rule 1) |
| failedInclusiveTerminalOfferSealsMemoBeforeALaterSkip | RETIRED (no inclusive terminal boundary: dense journalSeq, cpp M2) |
| inclusiveTerminalInEarlierRecordingDoesNotBlockLaterAppliedByteResume | RETIRED (as above; per-recording proofs, ResumeMemoTest.lastRecordingWithAProofWins) |
| duplicateOfEarlierUnsentTradeCannotStartALaterRecordingByteProof | RETIRED (5.2 rule 1) |

## BridgeEndToEndTest (9 scenarios), the archive half

| Java scenario | Verdict | C++ |
|---|---|---|
| byteResumeSurvivesAToBToAWithDifferentRealFrameBoundaries (:484) | CARRIED | IT perSourceMemoSurvivesFailBack |
| emptyHeadStoppedEofAndActiveFrontierContinueIntoSuccessor (:560) | CARRIED | IT emptyGenesisRecordingThenData, IT chainFollowAcrossRecordingsAndSuccessor |
| earlierInclusiveTerminalDoesNotForceRescanningLaterAppliedRecording (:607) | RETIRED (no inclusive terminal) | ResumeMemoTest.lastRecordingWithAProofWins |
| backlogBatchesApplyInOrderAndSessionSurvivesIdlePastTimeout (:660) | S9c (core session, R-K6) | the duty bound half: IT appliedHeadKeepsDutyBounded |
| stoppedReplayEndingBeforeCatalogEofIsAVisibleFailure (:697) | CARRIED | FollowPolicyTest.stoppedReplayBeforeEofIsFailure |
| rejectedRememberedOffsetClearsBothProofsAndNextReplayStartsSafely (:742) | CARRIED | IT rejectedRememberedPositionClearsProofs, ResumeMemoTest.rejectedRememberedPositionClearsProofs |
| replayCapacityRejectionPreservesValidByteAndDrainedPrefixProofs (:791) | CARRIED | IT capacityRejectionKeepsProofs, ResumeMemoTest.capacityRejectionKeepsProofs |
| bridgeSettlesReleasesAndResumesAcrossARestart (:837) | S9c (R-K1) | (none in S9a) |
| bridgeRecoversWhenJournalSourceDiesMidLiveFollow (:918) | CARRIED for detection; recovery is S9c (R-K3) | IT imageLostMidFollowEndsSync |

## Not the archive side (S9b, S9c)

`BridgeFilterTest` (6) -> S9b cursor table; translator tests (12) -> S9b/S9h; `BridgeMetricsServer`
tests (2) and `LatencyHistogramTest` (3) -> S9c; `AeDumpTest` (4) moves with the tool (cpp P7).
