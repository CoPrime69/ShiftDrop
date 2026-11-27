import XCTest
@testable import ShiftDrop

final class MediaParseTests: XCTestCase {
    private let videoLog = [
        "Input #0, mov,mp4,m4a,3gp,3g2,mj2, from 'clip.mp4':",
        "  Duration: 00:01:23.45, start: 0.000000, bitrate: 1234 kb/s",
        "  Stream #0:0(und): Video: h264 (High) (avc1 / 0x31637661), yuv420p(progressive), 1920x1080 [SAR 1:1 DAR 16:9], 5000 kb/s, 29.97 fps, 29.97 tbr, 30k tbn (default)",
        "  Stream #0:1(und): Audio: aac (LC) (mp4a / 0x6134706D), 48000 Hz, stereo, fltp, 128 kb/s (default)",
    ]

    func testParseInfoVideoAndAudio() {
        let info = Media.parseInfo(videoLog)
        XCTAssertEqual(info.durationUs, 83_450_000)
        XCTAssertEqual(info.videoCodec, "h264")
        XCTAssertEqual(info.audioCodec, "aac")
        XCTAssertEqual(info.width, 1920)
        XCTAssertEqual(info.height, 1080)
        XCTAssertEqual(info.fps, 29.97, accuracy: 0.001)
    }

    func testParseInfoAudioOnlyHasNoVideo() {
        let info = Media.parseInfo(["  Duration: 00:03:00.00, start: 0.0, bitrate: 192 kb/s",
                                    "  Stream #0:0: Audio: mp3, 44100 Hz, stereo, fltp, 192 kb/s"])
        XCTAssertEqual(info.durationUs, 180_000_000)
        XCTAssertEqual(info.videoCodec, "")
        XCTAssertEqual(info.audioCodec, "mp3")
    }

    func testParseInfoIgnoresCoverArt() {
        let info = Media.parseInfo(["  Duration: 00:00:10.00, start: 0.0, bitrate: 128 kb/s",
                                    "  Stream #0:0: Audio: mp3, 44100 Hz, stereo, fltp, 128 kb/s",
                                    "  Stream #0:1: Video: mjpeg (Baseline), yuvj420p, 600x600, 90k tbr (attached pic)"])
        XCTAssertEqual(info.videoCodec, "")
    }

    func testParseInfoUnknownDurationStaysNegative() {
        XCTAssertLessThan(Media.parseInfo(["  Duration: N/A, bitrate: N/A"]).durationUs, 0)
    }

    func testParseDuration() {
        XCTAssertEqual(Media.parseDuration("  Duration: 01:00:00.50, start: 0"), 3_600_500_000)
        XCTAssertNil(Media.parseDuration("  Duration: N/A, start: 0"))
        XCTAssertNil(Media.parseDuration("no duration here"))
    }

    func testParseProgress() {
        XCTAssertEqual(Media.parseProgress("out_time_us=1500000"), 1_500_000)
        XCTAssertEqual(Media.parseProgress("out_time_us=0"), 0)
        XCTAssertNil(Media.parseProgress("out_time_us=N/A"))
        XCTAssertNil(Media.parseProgress("out_time_us=-9223372036854775807"))
        XCTAssertNil(Media.parseProgress("frame=10"))
    }
}

final class PlanSizeFitTests: XCTestCase {
    private func video(seconds: Int64, height: Int = 1080) -> MediaInfo {
        var i = MediaInfo()
        i.durationUs = seconds * 1_000_000; i.videoCodec = "h264"; i.audioCodec = "aac"
        i.width = height * 16 / 9; i.height = height; i.fps = 30
        return i
    }

    func testNeedsADurationAndATarget() {
        XCTAssertNil(Media.planSizeFit(targetBytes: 10_000_000, info: MediaInfo()))   // duration unknown
        XCTAssertNil(Media.planSizeFit(targetBytes: 0, info: video(seconds: 60)))
    }

    func testTenMegabytesInAMinuteDropsTo480p() throws {
        let fit = try XCTUnwrap(Media.planSizeFit(targetBytes: 10_000_000, info: video(seconds: 60)))
        XCTAssertEqual(fit.audioBps, 96_000)
        XCTAssertEqual(fit.videoBps, 1_184_000)   // 10 MB * 8 * 0.96 / 60 s, minus the audio
        XCTAssertEqual(fit.height, 480)
    }

    func testRoomyTargetKeepsTheSourceHeight() throws {
        let fit = try XCTUnwrap(Media.planSizeFit(targetBytes: 2_000_000_000, info: video(seconds: 60, height: 360)))
        XCTAssertEqual(fit.height, 0)   // 0 = no scaling; never upscales
        XCTAssertEqual(fit.audioBps, 128_000)
    }

    func testTargetTooSmallForVideoIsRefused() {
        XCTAssertNil(Media.planSizeFit(targetBytes: 100_000, info: video(seconds: 600)))
    }

    func testAudioOnly() throws {
        var a = MediaInfo(); a.durationUs = 60_000_000; a.audioCodec = "mp3"
        XCTAssertEqual(try XCTUnwrap(Media.planSizeFit(targetBytes: 1_000_000, info: a)).audioBps, 128_000)
        XCTAssertNil(Media.planSizeFit(targetBytes: 100_000, info: { var b = a; b.durationUs = 600_000_000; return b }()))
    }
}

final class SubtitlesTests: XCTestCase {
    private let srt = "\u{FEFF}1\r\n00:00:01,000 --> 00:00:02,500\r\nHello <i>there</i>\r\n\r\n2\r\n00:01:00,000 --> 01:00:00,250\r\nSecond\r\nline\r\n"

    func testParseSrtWithBomAndCrlf() {
        let cues = Subtitles.parse(srt)
        XCTAssertEqual(cues.count, 2)
        XCTAssertEqual(cues[0].start, 1.0, accuracy: 1e-9)
        XCTAssertEqual(cues[0].end, 2.5, accuracy: 1e-9)
        XCTAssertEqual(cues[0].text, "Hello <i>there</i>")
        XCTAssertEqual(cues[1].end, 3600.25, accuracy: 1e-9)
        XCTAssertEqual(cues[1].text, "Second\nline")
    }

    func testSecondsAcceptsAllStampForms() {
        XCTAssertEqual(Subtitles.seconds("00:01:02,500"), 62.5)
        XCTAssertEqual(Subtitles.seconds("01:02.500"), 62.5)
        XCTAssertEqual(Subtitles.seconds("1:02:03.4")!, 3723.4, accuracy: 1e-9)
        XCTAssertNil(Subtitles.seconds("soon"))
    }

    func testWriteSrtAndVtt() {
        let cues = Subtitles.parse(srt)
        XCTAssertTrue(Subtitles.srt(cues).hasPrefix("1\n00:00:01,000 --> 00:00:02,500\nHello <i>there</i>\n"))
        let vtt = Subtitles.vtt(cues)
        XCTAssertTrue(vtt.hasPrefix("WEBVTT\n\n00:00:01.000 --> 00:00:02.500\n"))
        XCTAssertEqual(Subtitles.parse(vtt).count, 2)   // reads its own output
    }

    func testTxtStripsTagsAndJoinsLines() {
        XCTAssertEqual(Subtitles.txt(Subtitles.parse(srt)), "Hello there\nSecond line\n")
    }

    func testBlocksWithoutATimingLineAreSkipped() {
        XCTAssertTrue(Subtitles.parse("WEBVTT\n\nNOTE just a comment\n").isEmpty)
    }
}

final class SheetsTests: XCTestCase {
    func testParseQuotedFields() {
        let rows = Sheets.parse("a,\"b,c\",\"say \"\"hi\"\"\"\n1,2,3\n", delimiter: ",")
        XCTAssertEqual(rows, [["a", "b,c", "say \"hi\""], ["1", "2", "3"]])
    }

    func testParseNewlineInsideQuotesAndCrlf() {
        let rows = Sheets.parse("\"two\nlines\",x\r\ny,z", delimiter: ",")
        XCTAssertEqual(rows, [["two\nlines", "x"], ["y", "z"]])
    }

    func testParseTsvAndBom() {
        XCTAssertEqual(Sheets.parse("\u{FEFF}a\tb\n", delimiter: "\t"), [["a", "b"]])
    }

    func testWriteQuotesOnlyWhatNeedsIt() {
        XCTAssertEqual(Sheets.write([["a", "b,c", "q\"q"], ["1", "", "3"]], delimiter: ","), "a,\"b,c\",\"q\"\"q\"\n1,,3\n")
    }

    func testCsvToTsvRoundTrip() {
        let rows = Sheets.parse("name,note\nAnn,\"x,y\"\n", delimiter: ",")
        XCTAssertEqual(Sheets.write(rows, delimiter: "\t"), "name\tnote\nAnn\tx,y\n")
    }
}

final class OutputPathTests: XCTestCase {
    private let dir = URL(fileURLWithPath: "/tmp/out")

    func testFirstCandidateHasNoNumber() {
        XCTAssertEqual(OutputPath.candidate(dir: dir, stem: "photo", suffix: "converted", ext: "png", n: 1).path, "/tmp/out/photo (converted).png")
    }

    func testLaterCandidatesCountFromTwo() {
        XCTAssertEqual(OutputPath.candidate(dir: dir, stem: "photo", suffix: "converted", ext: "png", n: 2).lastPathComponent, "photo (converted 2).png")
        XCTAssertEqual(OutputPath.candidate(dir: dir, stem: "photo", suffix: "converted", ext: "png", n: 10).lastPathComponent, "photo (converted 10).png")
    }

    func testNoExtension() {
        XCTAssertEqual(OutputPath.candidate(dir: dir, stem: "folder", suffix: "zipped", ext: "", n: 1).lastPathComponent, "folder (zipped)")
    }
}

final class BuiltInRecipesTests: XCTestCase {
    func testBuiltInsLoadFromTheSharedJson() {
        let all = RecipeStore.all
        XCTAssertTrue(all.count > 50)
        XCTAssertEqual(Set(all.map(\.id)).count, all.count)   // ids are unique
        XCTAssertTrue(all.allSatisfy { !$0.steps.isEmpty && !$0.label.isEmpty })
        XCTAssertEqual(RecipeStore.find("media-under-10mb")?.label, "Under 10 MB")
        XCTAssertEqual(RecipeStore.find("to-png")?.steps.first?.action, "image.encode")
    }
}
