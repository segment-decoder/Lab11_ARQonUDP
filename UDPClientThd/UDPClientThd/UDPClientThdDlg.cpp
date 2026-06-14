// UDPClientThdDlg.cpp: 대화상자와 ARQ 프로토콜 동작을 구현한 파일입니다.
#include "pch.h"
#include "framework.h"
#include "UDPClientThd.h"
#include "UDPClientThdDlg.h"
#include "afxdialogex.h"

#pragma comment(lib, "ws2_32.lib")

#ifdef _DEBUG
#define new DEBUG_NEW
#endif

CCriticalSection tx_cs;
CCriticalSection rx_cs;

BOOL g_isTrimmingInput = FALSE; // 처리 상태를 나타내는 boolean 변수입니다.
BOOL g_verboseLog = FALSE; // 시작 시 요약 로그 모드로 둡니다.

void AppendEditText(CEdit& edit, const CString& text) // Edit Control 끝에 로그 문자열을 추가하는 함수입니다.
{
	int len = edit.GetWindowTextLengthW(); // 입력창의 현재 문자열을 읽습니다.
	edit.SetSel(len, len); // Edit Control의 커서 위치를 지정합니다.
	edit.ReplaceSel(text); // Edit Control의 현재 커서 위치에 문자열을 붙입니다.
}

CString CompactActionLabel(LPCTSTR action) // 요약 로그에서 송수신 동작을 짧게 표시하기 위한 함수입니다.
{
	if (lstrcmp(action, _T("SEND")) == 0)
		return _T("▶ 송신");
	if (lstrcmp(action, _T("RECV")) == 0)
		return _T("◀ 수신");
	if (lstrcmp(action, _T("CREATE")) == 0)
		return _T("＋ 생성");
	return action;
}

int GetUtf8ByteCount(const CStringW& text) // CStringW가 UTF-8로 변환될 때 필요한 Byte 수를 계산합니다.
{
	if (text.IsEmpty()) // 처리할 데이터가 없는지 확인합니다.
		return 0;

	return WideCharToMultiByte(CP_UTF8, 0, text, text.GetLength(), NULL, 0, NULL, NULL);
}

void AddChecksumWord(DWORD& sum, WORD word) // 16bit word를 checksum 누적합에 더하고 carry를 접습니다.
{
	sum += word;
	while ((sum >> 16) != 0) // 조건이 만족되는 동안 반복합니다.
		sum = (sum & 0xFFFF) + (sum >> 16);
}

void AddChecksumInt(DWORD& sum, int value) // 32bit header 값을 16bit word 두 개로 나누어 checksum에 반영합니다.
{
	DWORD unsignedValue = (DWORD)value;
	AddChecksumWord(sum, (WORD)((unsignedValue >> 16) & 0xFFFF)); // 16bit word를 checksum 누적합에 더하고 carry를 접습니다.
	AddChecksumWord(sum, (WORD)(unsignedValue & 0xFFFF)); // 16bit word를 checksum 누적합에 더하고 carry를 접습니다.
}

void AddChecksumPayload(DWORD& sum, const BYTE* payload, int payloadLen) // payload Byte들을 16bit word 단위로 checksum에 반영합니다.
{
	for (int index = 0; index < payloadLen; index += 2) // 필요한 범위만큼 반복합니다.
	{
		WORD word = ((WORD)payload[index]) << 8; // checksum에 더할 16bit word입니다.
		if (index + 1 < payloadLen)
			word |= payload[index + 1];

		AddChecksumWord(sum, word); // 16bit word를 checksum 누적합에 더하고 carry를 접습니다.
	}
}

int CalculateFrameChecksum(const Frame& frame) // Frame header와 payload 기준으로 16bit one's complement checksum을 계산합니다.
{
	DWORD sum = 0; // checksum 계산에 사용할 누적합입니다.
	AddChecksumInt(sum, frame.seq_num); // 32bit header 값을 16bit word 두 개로 나누어 checksum에 반영합니다.
	AddChecksumInt(sum, frame.ack_num); // 32bit header 값을 16bit word 두 개로 나누어 checksum에 반영합니다.
	AddChecksumInt(sum, frame.msg_id); // 32bit header 값을 16bit word 두 개로 나누어 checksum에 반영합니다.
	AddChecksumInt(sum, frame.frag_index); // 32bit header 값을 16bit word 두 개로 나누어 checksum에 반영합니다.
	AddChecksumInt(sum, frame.frag_count); // 32bit header 값을 16bit word 두 개로 나누어 checksum에 반영합니다.
	AddChecksumInt(sum, frame.payload_len); // 32bit header 값을 16bit word 두 개로 나누어 checksum에 반영합니다.
	AddChecksumPayload(sum, frame.payload, frame.payload_len); // payload Byte들을 16bit word 단위로 checksum에 반영합니다.
	while ((sum >> 16) != 0) // 조건이 만족되는 동안 반복합니다.
		sum = (sum & 0xFFFF) + (sum >> 16);

	return (int)(~sum & 0xFFFF);
}

BOOL VerifyFrameChecksum(const Frame& frame, int& calculatedChecksum) // 수신 checksum과 재계산 checksum이 같은지 검증합니다.
{
	calculatedChecksum = CalculateFrameChecksum(frame); // Frame header와 payload 기준으로 16bit one's complement checksum을 계산합니다.
	return frame.checksum == calculatedChecksum;
}

BOOL IsAckOnlyFrame(const Frame& frame) // payload 없이 ACK만 전달하는 제어 Frame인지 확인합니다.
{
	return frame.seq_num == 0 && frame.ack_num > 0 && frame.msg_id == 0 && frame.frag_index == 0 && frame.frag_count == 0 && frame.payload_len == 0;
}

void AppendChecksumLog(CEdit& edit, LPCTSTR result, const Frame& frame, int calculatedChecksum) // checksum 검증 결과를 패킷 로그창에 남깁니다.
{
	if (!g_verboseLog && lstrcmp(result, _T("OK")) == 0)
		return;

	CString log; // 화면 출력에 사용할 문자열 변수입니다.
	if (IsAckOnlyFrame(frame)) // payload 없이 ACK만 전달하는 제어 Frame인지 확인합니다.
	{
		log.Format(_T("[CHECKSUM %s] ack_only ack=%d recv=%d calc=%d\r\n"), result, frame.ack_num, frame.checksum, calculatedChecksum); // 화면에 표시할 로그 문자열을 구성합니다.
		AppendEditText(edit, log); // Edit Control 끝에 로그 문자열을 추가하는 함수입니다.
		return;
	}

	log.Format(_T("[CHECKSUM %s] msg=%d frag=%d/%d recv=%d calc=%d\r\n"), result, frame.msg_id, frame.frag_index + 1, frame.frag_count, frame.checksum, calculatedChecksum); // 화면에 표시할 로그 문자열을 구성합니다.
	AppendEditText(edit, log); // Edit Control 끝에 로그 문자열을 추가하는 함수입니다.
}

BOOL CorruptFrameForChecksumDemo(Frame& frame) // checksum 실패 시연을 위해 payload 일부를 고의로 손상합니다.
{
	if (frame.payload_len <= 0)
		return FALSE;

	frame.payload[0] ^= 0x01;
	return TRUE;
}

void AppendCorruptPacketLog(CEdit& edit, const Frame& frame) // 고의로 손상한 Frame 정보를 로그에 남깁니다.
{
	CString log; // 화면 출력에 사용할 문자열 변수입니다.
	log.Format(_T("[CORRUPT PACKET] msg=%d frag=%d/%d payload_byte=0 checksum_kept=%d\r\n"), frame.msg_id, frame.frag_index + 1, frame.frag_count, frame.checksum); // 화면에 표시할 로그 문자열을 구성합니다.
	AppendEditText(edit, log); // Edit Control 끝에 로그 문자열을 추가하는 함수입니다.
}

BOOL InjectFrameError(Frame& frame, int errorType, CEdit& edit) // 선택한 오류를 다음 송신 Frame에 1회 주입합니다.
{
	CString log; // 화면 출력에 사용할 문자열 변수입니다.
	switch (errorType) // 선택된 오류 종류에 따라 처리 방식을 나눕니다.
	{
	case 1: // checksum 오류를 주입하는 경우입니다.
		if (frame.payload_len > 0)
			frame.payload[0] ^= 0x01;
		else // 앞 조건에 해당하지 않는 경우를 처리합니다.
			frame.checksum ^= 0x1;
		log.Format(_T("[주입] Checksum 오류 seq=%d\r\n"), frame.seq_num); // 화면에 표시할 로그 문자열을 구성합니다.
		AppendEditText(edit, log); // Edit Control 끝에 로그 문자열을 추가하는 함수입니다.
		return FALSE;
	case 2: // seq 번호 오류를 주입하는 경우입니다.
		frame.seq_num += 5;
		frame.checksum = CalculateFrameChecksum(frame); // Frame header와 payload 기준으로 16bit one's complement checksum을 계산합니다.
		log.Format(_T("[주입] Seq 번호 오류 seq->%d\r\n"), frame.seq_num); // 화면에 표시할 로그 문자열을 구성합니다.
		AppendEditText(edit, log); // Edit Control 끝에 로그 문자열을 추가하는 함수입니다.
		return FALSE;
	case 3: // ACK 번호 오류를 주입하는 경우입니다.
		frame.ack_num += 100;
		frame.checksum = CalculateFrameChecksum(frame); // Frame header와 payload 기준으로 16bit one's complement checksum을 계산합니다.
		log.Format(_T("[주입] ACK 번호 오류 ack->%d\r\n"), frame.ack_num); // 화면에 표시할 로그 문자열을 구성합니다.
		AppendEditText(edit, log); // Edit Control 끝에 로그 문자열을 추가하는 함수입니다.
		return FALSE;
	case 4: // payload 길이 오류를 주입하는 경우입니다.
		frame.payload_len = FRAME_PAYLOAD_SIZE + 10; // 과제 조건에 맞춰 Frame payload를 16Byte로 고정합니다.
		frame.checksum = CalculateFrameChecksum(frame); // Frame header와 payload 기준으로 16bit one's complement checksum을 계산합니다.
		log.Format(_T("[주입] Payload 길이 오류 payload_len->%d\r\n"), frame.payload_len); // 화면에 표시할 로그 문자열을 구성합니다.
		AppendEditText(edit, log); // Edit Control 끝에 로그 문자열을 추가하는 함수입니다.
		return FALSE;
	case 5: // fragment header 오류를 주입하는 경우입니다.
		frame.frag_index = frame.frag_count + 5;
		frame.checksum = CalculateFrameChecksum(frame); // Frame header와 payload 기준으로 16bit one's complement checksum을 계산합니다.
		log.Format(_T("[주입] 세그먼트 헤더 오류 frag_index->%d\r\n"), frame.frag_index); // 화면에 표시할 로그 문자열을 구성합니다.
		AppendEditText(edit, log); // Edit Control 끝에 로그 문자열을 추가하는 함수입니다.
		return FALSE;
	case 6: // reassembly 미완성 상황을 만드는 경우입니다.
		frame.frag_count += 1;
		frame.checksum = CalculateFrameChecksum(frame); // Frame header와 payload 기준으로 16bit one's complement checksum을 계산합니다.
		log.Format(_T("[주입] 재조립 미완성(조각수 위조) frag_count->%d\r\n"), frame.frag_count); // 화면에 표시할 로그 문자열을 구성합니다.
		AppendEditText(edit, log); // Edit Control 끝에 로그 문자열을 추가하는 함수입니다.
		return FALSE;
	case 7: // 패킷 손실을 시연하는 경우입니다.
		log.Format(_T("[주입] 패킷 손실 seq=%d 전송 생략(타임아웃/재전송 유발)\r\n"), frame.seq_num); // 화면에 표시할 로그 문자열을 구성합니다.
		AppendEditText(edit, log); // Edit Control 끝에 로그 문자열을 추가하는 함수입니다.
		return TRUE;
	default: // 정의되지 않은 경우의 기본 처리를 수행합니다.
		return FALSE;
	}
}

void ApplyXorCipher(Frame& frame) // payload에 XOR 변환을 적용하며 송신 시 암호화, 수신 시 복호화에 같이 사용합니다.
{
	for (int index = 0; index < frame.payload_len; index++) // 필요한 범위만큼 반복합니다.
		frame.payload[index] = frame.payload[index] ^ XOR_KEY; // payload XOR 변환에 사용할 1Byte key입니다.
}

void AppendXorCipherLog(CEdit& edit, LPCTSTR action, const Frame& frame) // XOR 변환 수행 결과를 상세 로그에 남깁니다.
{
	if (!g_verboseLog)
		return;

	CString log; // 화면 출력에 사용할 문자열 변수입니다.
	log.Format(_T("[XOR %s] msg=%d frag=%d/%d key=0x%02X payload_len=%d\r\n"), action, frame.msg_id, frame.frag_index + 1, frame.frag_count, XOR_KEY, frame.payload_len); // payload XOR 변환에 사용할 1Byte key입니다.
	AppendEditText(edit, log); // Edit Control 끝에 로그 문자열을 추가하는 함수입니다.
}

Frame MakeAckOnlyFrame(int ackNum) // 정상 수신한 Frame 번호만 담은 ACK-only Frame을 만듭니다.
{
	Frame ackFrame; // ACK-only Frame을 담을 변수입니다.
	ackFrame.seq_num = 0; // ACK-only Frame은 데이터 seq 번호를 사용하지 않습니다.
	ackFrame.ack_num = ackNum; // 상대에게 알려줄 ACK 번호를 기록합니다.
	ackFrame.checksum = CalculateFrameChecksum(ackFrame); // Frame header와 payload 기준으로 16bit one's complement checksum을 계산합니다.
	return ackFrame;
}

void QueueAckOnlyFrame(CList<Frame, Frame&>* pList, int ackNum, CEdit& edit) // 송신 또는 수신 Frame 큐를 가리키는 포인터입니다.
{
	if (ackNum <= 0)
		return;

	Frame ackFrame = MakeAckOnlyFrame(ackNum); // 정상 수신한 Frame 번호만 담은 ACK-only Frame을 만듭니다.
	tx_cs.Lock(); // 공유 자료구조 접근을 보호하기 위해 lock을 잡습니다.
	pList->AddHead(ackFrame); // ACK 우선 전송을 위해 리스트 앞쪽에 추가합니다.
	tx_cs.Unlock(); // 공유 자료구조 접근이 끝나 lock을 해제합니다.

	if (g_verboseLog)
	{
		CString log; // 화면 출력에 사용할 문자열 변수입니다.
		log.Format(_T("[ACK QUEUE] ack=%d checksum=%d\r\n"), ackFrame.ack_num, ackFrame.checksum); // 화면에 표시할 로그 문자열을 구성합니다.
		AppendEditText(edit, log); // Edit Control 끝에 로그 문자열을 추가하는 함수입니다.
	}
}

BOOL TakeAckOnlyFrame(CList<Frame, Frame&>* pList, Frame& frame) // 송신 또는 수신 Frame 큐를 가리키는 포인터입니다.
{
	POSITION pos = pList->GetHeadPosition(); // MFC 리스트 순회를 시작할 위치를 얻습니다.
	while (pos != NULL) // 조건이 만족되는 동안 반복합니다.
	{
		POSITION currentPos = pos; // MFC 리스트 순회 위치를 저장합니다.
		Frame currentFrame = pList->GetNext(pos); // 현재 위치의 항목을 가져오고 다음 위치로 이동합니다.
		if (IsAckOnlyFrame(currentFrame)) // payload 없이 ACK만 전달하는 제어 Frame인지 확인합니다.
		{
			frame = currentFrame;
			pList->RemoveAt(currentPos); // 처리한 항목을 리스트에서 제거합니다.
			return TRUE;
		}
	}

	return FALSE;
}

void RefreshFrameAckAndChecksum(Frame& frame, int lastAckNum) // 송신 직전 최신 ACK 값을 반영하고 checksum을 다시 계산합니다.
{
	if (!IsAckOnlyFrame(frame)) // payload 없이 ACK만 전달하는 제어 Frame인지 확인합니다.
		frame.ack_num = lastAckNum; // 마지막 정상 수신 번호를 piggyback ACK로 넣습니다.

	frame.checksum = CalculateFrameChecksum(frame); // Frame header와 payload 기준으로 16bit one's complement checksum을 계산합니다.
}

CString Utf8BytesToText(const BYTE* payload, int payloadLen, int maxBytes) // UTF-8 payload Byte 배열을 화면 출력용 CString으로 복원합니다.
{
	if (payloadLen <= 0 || payloadLen > maxBytes)
		return _T("");

	int wideChars = MultiByteToWideChar(CP_UTF8, 0, (LPCCH)payload, payloadLen, NULL, 0); // UTF-8 Byte 배열을 Unicode 문자열로 변환합니다.
	if (wideChars <= 0)
		return _T("");

	CStringW wideText;
	LPWSTR buffer = wideText.GetBuffer(wideChars); // 문자 변환 결과를 받을 CString 버퍼를 확보합니다.
	MultiByteToWideChar(CP_UTF8, 0, (LPCCH)payload, payloadLen, buffer, wideChars); // UTF-8 Byte 배열을 Unicode 문자열로 변환합니다.
	wideText.ReleaseBuffer(wideChars); // CString 버퍼 사용을 마치고 길이를 확정합니다.
	return CString(wideText);
}

BOOL LimitTextToMessageSize(const CString& source, CString& limitedText) // 입력 문자열을 256Byte 제한 안에 들어가도록 자릅니다.
{
	CStringW sourceText(source);
	CStringW limitedWideText;
	int usedBytes = 0; // 계산 또는 상태 추적에 사용할 정수 변수입니다.
	int index = 0; // 계산 또는 상태 추적에 사용할 정수 변수입니다.

	while (index < sourceText.GetLength()) // 조건이 만족되는 동안 반복합니다.
	{
		int unitCount = 1; // 계산 또는 상태 추적에 사용할 정수 변수입니다.
		WCHAR currentChar = sourceText[index];

		if (currentChar >= 0xD800 && currentChar <= 0xDBFF && index + 1 < sourceText.GetLength()) // UTF-16 surrogate pair의 앞쪽 코드 유닛인지 확인합니다.
		{
			WCHAR nextChar = sourceText[index + 1];
			if (nextChar >= 0xDC00 && nextChar <= 0xDFFF) // surrogate pair의 뒤쪽 코드 유닛인지 확인합니다.
				unitCount = 2;
		}

		CStringW unitText = sourceText.Mid(index, unitCount);
		int unitBytes = WideCharToMultiByte(CP_UTF8, 0, unitText, unitCount, NULL, 0, NULL, NULL); // Unicode 문자열을 UTF-8 Byte 기준으로 변환하거나 길이를 계산합니다.
		if (usedBytes + unitBytes > MAX_MESSAGE_BYTES)
			break; // 현재 반복 또는 분기 처리를 중단합니다.

		limitedWideText += unitText;
		usedBytes += unitBytes;
		index += unitCount; // 다음 문자 단위로 이동합니다.
	}

	limitedText = CString(limitedWideText);
	return sourceText.GetLength() != limitedWideText.GetLength();
}

BOOL BuildFramesFromText(const CString& text, CList<Frame, Frame&>& frameList, int messageId, int& messageBytes) // 입력 문자열을 16Byte payload Frame들로 분할합니다.
{
	CStringW wideText(text);
	int utf8Bytes = GetUtf8ByteCount(wideText); // CStringW가 UTF-8로 변환될 때 필요한 Byte 수를 계산합니다.
	Frame currentFrame;
	int index = 0; // 계산 또는 상태 추적에 사용할 정수 변수입니다.
	int fragmentCount = 0; // 계산 또는 상태 추적에 사용할 정수 변수입니다.
	POSITION pos; // MFC 리스트 순회 위치를 저장합니다.
	int fragmentIndex = 0; // 계산 또는 상태 추적에 사용할 정수 변수입니다.

	frameList.RemoveAll(); // 이전 Frame 목록을 비우고 새 메시지를 준비합니다.
	messageBytes = utf8Bytes;

	if (utf8Bytes <= 0)
		return FALSE;

	if (utf8Bytes > MAX_MESSAGE_BYTES)
		return FALSE;

	while (index < wideText.GetLength()) // 조건이 만족되는 동안 반복합니다.
	{
		int unitCount = 1; // 계산 또는 상태 추적에 사용할 정수 변수입니다.
		WCHAR currentChar = wideText[index];

		if (currentChar >= 0xD800 && currentChar <= 0xDBFF && index + 1 < wideText.GetLength()) // UTF-16 surrogate pair의 앞쪽 코드 유닛인지 확인합니다.
		{
			WCHAR nextChar = wideText[index + 1];
			if (nextChar >= 0xDC00 && nextChar <= 0xDFFF) // surrogate pair의 뒤쪽 코드 유닛인지 확인합니다.
				unitCount = 2;
		}

		CStringW unitText = wideText.Mid(index, unitCount);
		int unitBytes = WideCharToMultiByte(CP_UTF8, 0, unitText, unitCount, NULL, 0, NULL, NULL); // Unicode 문자열을 UTF-8 Byte 기준으로 변환하거나 길이를 계산합니다.
		if (unitBytes <= 0 || unitBytes > FRAME_PAYLOAD_SIZE) // 과제 조건에 맞춰 Frame payload를 16Byte로 고정합니다.
			return FALSE;

		if (currentFrame.payload_len + unitBytes > FRAME_PAYLOAD_SIZE) // payload 길이가 허용 범위를 벗어났는지 검사합니다.
		{
			frameList.AddTail(currentFrame); // 리스트 뒤쪽에 새 항목을 추가합니다.
			fragmentCount++; // 생성된 fragment 개수를 증가시킵니다.
			currentFrame = Frame();
		}

		WideCharToMultiByte(CP_UTF8, 0, unitText, unitCount, (LPSTR)(currentFrame.payload + currentFrame.payload_len), FRAME_PAYLOAD_SIZE - currentFrame.payload_len, NULL, NULL); // Unicode 문자열을 UTF-8 Byte 기준으로 변환하거나 길이를 계산합니다.
		currentFrame.payload_len += unitBytes;
		index += unitCount; // 다음 문자 단위로 이동합니다.
	}

	if (currentFrame.payload_len > 0)
	{
		frameList.AddTail(currentFrame); // 리스트 뒤쪽에 새 항목을 추가합니다.
		fragmentCount++; // 생성된 fragment 개수를 증가시킵니다.
	}

	pos = frameList.GetHeadPosition(); // MFC 리스트 순회를 시작할 위치를 얻습니다.
	while (pos != NULL) // 조건이 만족되는 동안 반복합니다.
	{
		Frame& segmentFrame = frameList.GetNext(pos); // 현재 위치의 항목을 가져오고 다음 위치로 이동합니다.
		segmentFrame.msg_id = messageId;
		segmentFrame.frag_index = fragmentIndex;
		segmentFrame.frag_count = fragmentCount;
		fragmentIndex++; // 다음 fragment 번호로 이동합니다.
	}

	return !frameList.IsEmpty();
}

void ApplySeqAckToFrames(CList<Frame, Frame&>& frameList, int& nextSeqNum, int lastAckNum, CEdit& edit) // 각 Frame에 seq/ACK를 넣고 XOR 변환 후 checksum을 계산합니다.
{
	POSITION pos = frameList.GetHeadPosition(); // MFC 리스트 순회를 시작할 위치를 얻습니다.
	while (pos != NULL) // 조건이 만족되는 동안 반복합니다.
	{
		Frame& frame = frameList.GetNext(pos); // 현재 위치의 항목을 가져오고 다음 위치로 이동합니다.
		frame.seq_num = nextSeqNum; // 현재 Frame에 송신 seq 번호를 붙입니다.
		frame.ack_num = lastAckNum; // 마지막 정상 수신 번호를 piggyback ACK로 넣습니다.
		ApplyXorCipher(frame); // payload에 XOR 변환을 적용하며 송신 시 암호화, 수신 시 복호화에 같이 사용합니다.
		AppendXorCipherLog(edit, _T("ENCRYPT"), frame); // XOR 변환 수행 결과를 상세 로그에 남깁니다.
		frame.checksum = CalculateFrameChecksum(frame); // Frame header와 payload 기준으로 16bit one's complement checksum을 계산합니다.

		if (g_verboseLog)
		{
			CString log; // 화면 출력에 사용할 문자열 변수입니다.
			log.Format(_T("[PIGGYBACK] seq=%d ack=%d msg=%d frag=%d/%d checksum=%d\r\n"), frame.seq_num, frame.ack_num, frame.msg_id, frame.frag_index + 1, frame.frag_count, frame.checksum); // 화면에 표시할 로그 문자열을 구성합니다.
			AppendEditText(edit, log); // Edit Control 끝에 로그 문자열을 추가하는 함수입니다.
		}
		nextSeqNum++; // 다음 Frame에 사용할 seq 번호로 증가시킵니다.
	}
}

BOOL ProcessStopWaitAck(CEdit& edit, int ackNum, int peerSeqNum, BOOL ackOnly, int& lastReceivedAckNum, int lastSentSeqNum, BOOL& waitingAck, Frame& waitFrame, int& waitAckNum, int& retryCount, DWORD& lastSendTick) // 수신 ACK로 Stop-and-Wait 대기 상태를 갱신합니다.
{
	if (ackNum <= 0)
		return FALSE;

	CString ackType = ackOnly ? _T("ack_only") : _T("piggyback"); // 화면 출력에 사용할 문자열 변수입니다.
	if (ackNum > lastSentSeqNum)
	{
		CString ackWarnLog; // 화면 출력에 사용할 문자열 변수입니다.
		ackWarnLog.Format(_T("[ACK WARN] ack=%d last_sent=%d type=%s reason=future\r\n"), ackNum, lastSentSeqNum, ackType.GetString()); // 화면에 표시할 로그 문자열을 구성합니다.
		AppendEditText(edit, ackWarnLog); // Edit Control 끝에 로그 문자열을 추가하는 함수입니다.
		return FALSE;
	}

	BOOL isDuplicate = FALSE; // 처리 상태를 나타내는 boolean 변수입니다.
	BOOL completedWait = FALSE; // 처리 상태를 나타내는 boolean 변수입니다.
	int completedSeq = 0; // 계산 또는 상태 추적에 사용할 정수 변수입니다.

	tx_cs.Lock(); // 공유 자료구조 접근을 보호하기 위해 lock을 잡습니다.
	if (ackNum <= lastReceivedAckNum)
		isDuplicate = TRUE;
	else // 앞 조건에 해당하지 않는 경우를 처리합니다.
		lastReceivedAckNum = ackNum;

	if (!isDuplicate && waitingAck && ackNum >= waitAckNum)
	{
		completedSeq = waitAckNum;
		waitingAck = FALSE;
		waitFrame = Frame();
		waitAckNum = 0;
		retryCount = 0;
		lastSendTick = 0;
		completedWait = TRUE;
	}
	tx_cs.Unlock(); // 공유 자료구조 접근이 끝나 lock을 해제합니다.

	if (isDuplicate)
	{
		if (g_verboseLog)
		{
			CString ackDupLog; // 화면 출력에 사용할 문자열 변수입니다.
			ackDupLog.Format(_T("[ACK DUP] ack=%d last_ack=%d type=%s\r\n"), ackNum, lastReceivedAckNum, ackType.GetString()); // 화면에 표시할 로그 문자열을 구성합니다.
			AppendEditText(edit, ackDupLog); // Edit Control 끝에 로그 문자열을 추가하는 함수입니다.
		}
		return FALSE;
	}

	if (g_verboseLog)
	{
		CString ackReceiveLog; // 화면 출력에 사용할 문자열 변수입니다.
		ackReceiveLog.Format(_T("[ACK RECV] ack=%d type=%s peer_seq=%d\r\n"), ackNum, ackType.GetString(), peerSeqNum); // 화면에 표시할 로그 문자열을 구성합니다.
		AppendEditText(edit, ackReceiveLog); // Edit Control 끝에 로그 문자열을 추가하는 함수입니다.
	}

	if (completedWait)
	{
		CString doneLog; // 화면 출력에 사용할 문자열 변수입니다.
		if (g_verboseLog)
			doneLog.Format(_T("[SW DONE] seq=%d ack=%d\r\n"), completedSeq, ackNum); // 화면에 표시할 로그 문자열을 구성합니다.
		else // 앞 조건에 해당하지 않는 경우를 처리합니다.
			doneLog.Format(_T("✓ ACK seq%d 전송완료\r\n"), completedSeq); // 화면에 표시할 로그 문자열을 구성합니다.
		AppendEditText(edit, doneLog); // Edit Control 끝에 로그 문자열을 추가하는 함수입니다.
	}

	return TRUE;
}

BOOL ProcessSeqAck(CEdit& edit, const Frame& frame, int& expectedSeqNum, int& lastAckNum, int& lastReceivedAckNum, int lastSentSeqNum, BOOL& waitingAck, Frame& waitFrame, int& waitAckNum, int& retryCount, DWORD& lastSendTick) // seq 번호와 piggyback ACK를 검증합니다.
{
	ProcessStopWaitAck(edit, frame.ack_num, frame.seq_num, FALSE, lastReceivedAckNum, lastSentSeqNum, waitingAck, waitFrame, waitAckNum, retryCount, lastSendTick); // 수신 ACK로 Stop-and-Wait 대기 상태를 갱신합니다.

	if (frame.seq_num == expectedSeqNum)
	{
		if (g_verboseLog)
		{
			CString seqOkLog; // 화면 출력에 사용할 문자열 변수입니다.
			seqOkLog.Format(_T("[SEQ OK] seq=%d expected=%d\r\n"), frame.seq_num, expectedSeqNum); // 화면에 표시할 로그 문자열을 구성합니다.
			AppendEditText(edit, seqOkLog); // Edit Control 끝에 로그 문자열을 추가하는 함수입니다.
		}

		lastAckNum = frame.seq_num; // 정상 수신한 Frame 번호를 ACK 값으로 갱신합니다.
		expectedSeqNum++; // 다음에 기대할 seq 번호로 이동합니다.

		if (g_verboseLog)
		{
			CString ackUpdateLog; // 화면 출력에 사용할 문자열 변수입니다.
			ackUpdateLog.Format(_T("[ACK UPDATE] ack=%d next_expected=%d\r\n"), lastAckNum, expectedSeqNum); // 화면에 표시할 로그 문자열을 구성합니다.
			AppendEditText(edit, ackUpdateLog); // Edit Control 끝에 로그 문자열을 추가하는 함수입니다.
		}
		return TRUE;
	}

	if (frame.seq_num < expectedSeqNum)
	{
		CString seqDupLog; // 화면 출력에 사용할 문자열 변수입니다.
		seqDupLog.Format(_T("[SEQ DUP] seq=%d expected=%d last_ack=%d\r\n"), frame.seq_num, expectedSeqNum, lastAckNum); // 화면에 표시할 로그 문자열을 구성합니다.
		AppendEditText(edit, seqDupLog); // Edit Control 끝에 로그 문자열을 추가하는 함수입니다.
		return FALSE;
	}

	CString seqWarnLog; // 화면 출력에 사용할 문자열 변수입니다.
	seqWarnLog.Format(_T("[SEQ WARN] seq=%d expected=%d reason=out_of_order\r\n"), frame.seq_num, expectedSeqNum); // 화면에 표시할 로그 문자열을 구성합니다.
	AppendEditText(edit, seqWarnLog); // Edit Control 끝에 로그 문자열을 추가하는 함수입니다.
	return FALSE;
}

CString FramePayloadToText(const Frame& frame) // payload를 로그 출력용 문자열로 바꿉니다.
{
	if (frame.payload_len <= 0 || frame.payload_len > FRAME_PAYLOAD_SIZE) // 과제 조건에 맞춰 Frame payload를 16Byte로 고정합니다.
		return _T("");

	return Utf8BytesToText(frame.payload, frame.payload_len, FRAME_PAYLOAD_SIZE); // UTF-8 payload Byte 배열을 화면 출력용 CString으로 복원합니다.
}

void AppendPacketLog(CEdit& edit, LPCTSTR action, const Frame& frame, int packetBytes) // Frame header와 payload 요약 정보를 로그에 남깁니다.
{
	if (IsAckOnlyFrame(frame)) // payload 없이 ACK만 전달하는 제어 Frame인지 확인합니다.
	{
		if (!g_verboseLog)
			return;

		CString ackLog; // 화면 출력에 사용할 문자열 변수입니다.
		ackLog.Format(_T("[%s ACK] packet_bytes=%d ack=%d checksum=%d\r\n"), action, packetBytes, frame.ack_num, frame.checksum); // 화면에 표시할 로그 문자열을 구성합니다.
		AppendEditText(edit, ackLog); // Edit Control 끝에 로그 문자열을 추가하는 함수입니다.
		return;
	}

	CString payloadText = FramePayloadToText(frame); // payload를 로그 출력용 문자열로 바꿉니다.
	CString log; // 화면 출력에 사용할 문자열 변수입니다.
	if (g_verboseLog)
		log.Format(_T("[%s PACKET] packet_bytes=%d seq=%d ack=%d checksum=%d msg=%d frag=%d/%d payload_len=%d payload=\"%s\"\r\n"), action, packetBytes, frame.seq_num, frame.ack_num, frame.checksum, frame.msg_id, frame.frag_index + 1, frame.frag_count, frame.payload_len, payloadText.GetString()); // 화면에 표시할 로그 문자열을 구성합니다.
	else // 앞 조건에 해당하지 않는 경우를 처리합니다.
		log.Format(_T("%s seq%d m%d(%d/%d) ck✓ \"%s\"\r\n"), CompactActionLabel(action).GetString(), frame.seq_num, frame.msg_id, frame.frag_index + 1, frame.frag_count, payloadText.GetString()); // 요약 로그에서 송수신 동작을 짧게 표시하기 위한 함수입니다.
	AppendEditText(edit, log); // Edit Control 끝에 로그 문자열을 추가하는 함수입니다.
}

void AppendSegmentLog(CEdit& edit, int messageId, int messageBytes, int fragmentCount)
{
	CString log; // 화면 출력에 사용할 문자열 변수입니다.
	if (g_verboseLog)
		log.Format(_T("[SEGMENT] msg=%d message_bytes=%d fragments=%d payload_limit=%d\r\n"), messageId, messageBytes, fragmentCount, FRAME_PAYLOAD_SIZE); // 과제 조건에 맞춰 Frame payload를 16Byte로 고정합니다.
	else // 앞 조건에 해당하지 않는 경우를 처리합니다.
		log.Format(_T("[분할] msg=%d %dB->%d조각\r\n"), messageId, messageBytes, fragmentCount); // 화면에 표시할 로그 문자열을 구성합니다.
	AppendEditText(edit, log); // Edit Control 끝에 로그 문자열을 추가하는 함수입니다.
}

BOOL IsValidSegmentHeader(const Frame& frame) // fragment header 값이 정상 범위인지 확인합니다.
{
	if (frame.msg_id <= 0)
		return FALSE;

	if (frame.frag_count <= 0)
		return FALSE;

	if (frame.frag_count > MAX_SEGMENT_COUNT)
		return FALSE;

	if (frame.frag_index < 0)
		return FALSE;

	if (frame.frag_index >= frame.frag_count)
		return FALSE;

	return TRUE;
}

POSITION FindReassemblyMessage(CList<ReassemblyMessage, ReassemblyMessage&>& reassemblyList, int messageId) // MFC 리스트 순회 위치를 저장합니다.
{
	POSITION pos = reassemblyList.GetHeadPosition(); // MFC 리스트 순회를 시작할 위치를 얻습니다.
	while (pos != NULL) // 조건이 만족되는 동안 반복합니다.
	{
		POSITION currentPos = pos; // MFC 리스트 순회 위치를 저장합니다.
		ReassemblyMessage& message = reassemblyList.GetNext(pos); // 현재 위치의 항목을 가져오고 다음 위치로 이동합니다.
		if (message.msg_id == messageId)
			return currentPos;
	}

	return NULL;
}

void AppendReassemblyDropLog(CEdit& edit, int messageId, LPCTSTR reason)
{
	CString log; // 화면 출력에 사용할 문자열 변수입니다.
	log.Format(_T("[REASSEMBLY DROP] msg=%d reason=%s\r\n"), messageId, reason); // 화면에 표시할 로그 문자열을 구성합니다.
	AppendEditText(edit, log); // Edit Control 끝에 로그 문자열을 추가하는 함수입니다.
}

void CleanupExpiredReassemblyMessages(CList<ReassemblyMessage, ReassemblyMessage&>& reassemblyList, CEdit& edit) // reassembly 상태를 담을 변수입니다.
{
	DWORD nowTick = GetTickCount(); // 현재 시간을 기록해 timeout 또는 만료 여부를 판단합니다.
	POSITION pos = reassemblyList.GetHeadPosition(); // MFC 리스트 순회를 시작할 위치를 얻습니다.

	while (pos != NULL) // 조건이 만족되는 동안 반복합니다.
	{
		POSITION removePos = pos; // MFC 리스트 순회 위치를 저장합니다.
		ReassemblyMessage& message = reassemblyList.GetNext(pos); // 현재 위치의 항목을 가져오고 다음 위치로 이동합니다.
		if (nowTick - message.last_update_tick > REASSEMBLY_TIMEOUT_MS) // 오래 남은 미완성 reassembly 버퍼를 정리할 기준 시간입니다.
		{
			AppendReassemblyDropLog(edit, message.msg_id, _T("incomplete timeout"));
			reassemblyList.RemoveAt(removePos); // 처리한 항목을 리스트에서 제거합니다.
		}
	}
}

BOOL BuildReassembledText(const ReassemblyMessage& message, CString& completedText) // reassembly 상태를 담을 변수입니다.
{
	BYTE payload[MAX_MESSAGE_BYTES]; // payload Byte 데이터를 담을 버퍼입니다.
	int offset = 0; // 계산 또는 상태 추적에 사용할 정수 변수입니다.

	memset(payload, 0, sizeof(payload)); // 버퍼를 0으로 초기화합니다.
	completedText.Empty();

	for (int index = 0; index < message.frag_count; index++) // 필요한 범위만큼 반복합니다.
	{
		if (!message.received[index])
			return FALSE;

		const Frame& frame = message.fragments[index]; // 리스트 안의 Frame을 직접 수정하기 위한 참조입니다.
		if (offset + frame.payload_len > MAX_MESSAGE_BYTES)
			return FALSE;

		memcpy(payload + offset, frame.payload, frame.payload_len); // fragment payload를 reassembly 버퍼에 복사합니다.
		offset += frame.payload_len; // reassembly 버퍼의 다음 저장 위치로 이동합니다.
	}

	completedText = Utf8BytesToText(payload, offset, MAX_MESSAGE_BYTES); // 한 번에 보낼 수 있는 원본 메시지 크기를 256Byte로 제한합니다.
	return !completedText.IsEmpty();
}

BOOL ProcessReassemblyFrame(CList<ReassemblyMessage, ReassemblyMessage&>& reassemblyList, CEdit& edit, const Frame& frame, CString& completedText) // 수신 fragment를 저장하고 모두 모이면 원본 메시지를 복원합니다.
{
	CleanupExpiredReassemblyMessages(reassemblyList, edit); // reassembly 상태를 담을 변수입니다.
	completedText.Empty();

	POSITION messagePos = FindReassemblyMessage(reassemblyList, frame.msg_id); // MFC 리스트 순회 위치를 저장합니다.
	if (messagePos == NULL)
	{
		ReassemblyMessage newMessage; // reassembly 상태를 담을 변수입니다.
		newMessage.msg_id = frame.msg_id;
		newMessage.frag_count = frame.frag_count;
		newMessage.last_update_tick = GetTickCount(); // 현재 시간을 기록해 timeout 또는 만료 여부를 판단합니다.
		messagePos = reassemblyList.AddTail(newMessage); // 리스트 뒤쪽에 새 항목을 추가합니다.
	}

	ReassemblyMessage& message = reassemblyList.GetAt(messagePos); // 현재 위치의 리스트 항목을 참조합니다.
	if (message.frag_count != frame.frag_count)
	{
		AppendReassemblyDropLog(edit, frame.msg_id, _T("fragment count mismatch"));
		return FALSE;
	}

	if (message.received[frame.frag_index])
	{
		AppendReassemblyDropLog(edit, frame.msg_id, _T("duplicate fragment"));
		return FALSE;
	}

	message.fragments[frame.frag_index] = frame;
	message.received[frame.frag_index] = TRUE;
	message.received_count++; // 도착한 fragment 개수를 증가시킵니다.
	message.total_payload_len += frame.payload_len;
	message.last_update_tick = GetTickCount(); // 현재 시간을 기록해 timeout 또는 만료 여부를 판단합니다.

	if (g_verboseLog)
	{
		CString storeLog; // 화면 출력에 사용할 문자열 변수입니다.
		storeLog.Format(_T("[REASSEMBLY STORE] msg=%d frag=%d/%d received=%d/%d\r\n"), frame.msg_id, frame.frag_index + 1, frame.frag_count, message.received_count, message.frag_count); // 화면에 표시할 로그 문자열을 구성합니다.
		AppendEditText(edit, storeLog); // Edit Control 끝에 로그 문자열을 추가하는 함수입니다.
	}

	if (message.received_count < message.frag_count)
	{
		CString waitLog; // 화면 출력에 사용할 문자열 변수입니다.
		waitLog.Format(_T("[REASSEMBLY WAIT] msg=%d received=%d/%d\r\n"), message.msg_id, message.received_count, message.frag_count); // 화면에 표시할 로그 문자열을 구성합니다.
		AppendEditText(edit, waitLog); // Edit Control 끝에 로그 문자열을 추가하는 함수입니다.
		return FALSE;
	}

	if (!BuildReassembledText(message, completedText))
	{
		AppendReassemblyDropLog(edit, frame.msg_id, _T("rebuild failed"));
		reassemblyList.RemoveAt(messagePos); // 처리한 항목을 리스트에서 제거합니다.
		return FALSE;
	}

	CString doneLog; // 화면 출력에 사용할 문자열 변수입니다.
	if (g_verboseLog)
		doneLog.Format(_T("[REASSEMBLY DONE] msg=%d fragments=%d bytes=%d\r\n"), message.msg_id, message.frag_count, message.total_payload_len); // 화면에 표시할 로그 문자열을 구성합니다.
	else // 앞 조건에 해당하지 않는 경우를 처리합니다.
		doneLog.Format(_T("✓ 재조립 msg%d 완료(%d조각)\r\n"), message.msg_id, message.frag_count); // 화면에 표시할 로그 문자열을 구성합니다.
	AppendEditText(edit, doneLog); // Edit Control 끝에 로그 문자열을 추가하는 함수입니다.
	reassemblyList.RemoveAt(messagePos); // 처리한 항목을 리스트에서 제거합니다.
	return TRUE;
}

UINT TXThread(LPVOID arg) // 송신 큐에서 Frame을 꺼내 보내고 timeout 시 재전송하는 스레드입니다.
{
	ThreadArg* pArg = (ThreadArg*)arg; // 스레드 인자를 ThreadArg 포인터로 변환합니다.
	CList<Frame, Frame&>* plist = pArg->pList; // Frame들을 임시로 저장할 MFC 리스트입니다.
	CUDPClientThdDlg* pDlg = (CUDPClientThdDlg*)pArg->pDlg;

	while (pArg->Thread_run) // 스레드 실행 여부를 나타내는 플래그입니다.
	{
		Frame frame; // 처리할 Frame을 임시로 담는 변수입니다.
		BOOL hasFrame = FALSE; // 처리 상태를 나타내는 boolean 변수입니다.
		BOOL isAckOnly = FALSE; // 처리 상태를 나타내는 boolean 변수입니다.
		BOOL isRetransmission = FALSE; // 처리 상태를 나타내는 boolean 변수입니다.
		int currentRetryCount = 0; // 계산 또는 상태 추적에 사용할 정수 변수입니다.
		CString stateLog; // 화면 출력에 사용할 문자열 변수입니다.

		tx_cs.Lock(); // 공유 자료구조 접근을 보호하기 위해 lock을 잡습니다.
		if (TakeAckOnlyFrame(plist, frame)) // 송신 큐에서 ACK-only Frame을 찾아 먼저 꺼냅니다.
		{
			RefreshFrameAckAndChecksum(frame, pDlg->m_lastAckNum); // 송신 직전 최신 ACK 값을 반영하고 checksum을 다시 계산합니다.
			hasFrame = TRUE;
			isAckOnly = TRUE;
		}
		else if (pDlg->m_waitingAck) // 앞 조건이 아니면 다음 조건을 검사합니다.
		{
			if (pDlg->m_timeoutFired) // TimerThread가 timeout을 감지했는지 확인합니다.
			{
				if (pDlg->m_retryCount < STOP_WAIT_MAX_RETRY) // 같은 Frame을 재전송할 최대 횟수입니다.
				{
					pDlg->m_retryCount++;
					pDlg->m_elapsedTimeoutMs = 0; // ACK 대기 시간을 처음부터 다시 측정합니다.
					pDlg->m_timeoutFired = FALSE; // timeout 플래그를 초기 상태로 돌립니다.
					frame = pDlg->m_waitFrame;
					RefreshFrameAckAndChecksum(frame, pDlg->m_lastAckNum); // 송신 직전 최신 ACK 값을 반영하고 checksum을 다시 계산합니다.
					pDlg->m_waitFrame = frame; // timeout 재전송을 위해 현재 Frame을 보관합니다.
					hasFrame = TRUE;
					isRetransmission = TRUE;
					currentRetryCount = pDlg->m_retryCount;
				}
				else // 앞 조건에 해당하지 않는 경우를 처리합니다.
				{
					int failedSeq = pDlg->m_waitAckNum; // 계산 또는 상태 추적에 사용할 정수 변수입니다.
					pDlg->m_waitingAck = FALSE; // ACK 대기 상태를 해제합니다.
					pDlg->m_waitFrame = Frame(); // timeout 재전송에 사용할 대기 Frame을 비워 둡니다.
					pDlg->m_waitAckNum = 0; // 현재 기다리는 ACK 번호가 없음을 표시합니다.
					pDlg->m_retryCount = 0; // 현재 Frame의 재전송 횟수를 0으로 시작합니다.
					pDlg->m_elapsedTimeoutMs = 0; // ACK 대기 시간을 처음부터 다시 측정합니다.
					pDlg->m_timeoutFired = FALSE; // timeout 플래그를 초기 상태로 돌립니다.
					stateLog.Format(_T("[SW FAIL] seq=%d retry=%d/%d\r\n"), failedSeq, STOP_WAIT_MAX_RETRY, STOP_WAIT_MAX_RETRY); // 같은 Frame을 재전송할 최대 횟수입니다.
				}
			}
		}
		else if (!plist->IsEmpty()) // 앞 조건이 아니면 다음 조건을 검사합니다.
		{
			frame = plist->RemoveHead(); // 처리한 항목을 리스트에서 제거합니다.
			RefreshFrameAckAndChecksum(frame, pDlg->m_lastAckNum); // 송신 직전 최신 ACK 값을 반영하고 checksum을 다시 계산합니다.
			pDlg->m_waitingAck = TRUE; // Stop-and-Wait 규칙에 따라 ACK 대기 상태로 들어갑니다.
			pDlg->m_waitFrame = frame; // timeout 재전송을 위해 현재 Frame을 보관합니다.
			pDlg->m_waitAckNum = frame.seq_num; // 현재 Frame의 ACK를 기다리도록 번호를 저장합니다.
			pDlg->m_retryCount = 0; // 현재 Frame의 재전송 횟수를 0으로 시작합니다.
			pDlg->m_elapsedTimeoutMs = 0; // ACK 대기 시간을 처음부터 다시 측정합니다.
			pDlg->m_timeoutFired = FALSE; // timeout 플래그를 초기 상태로 돌립니다.
			hasFrame = TRUE;
		}
		tx_cs.Unlock(); // 공유 자료구조 접근이 끝나 lock을 해제합니다.

		if (!stateLog.IsEmpty()) // 처리할 데이터가 없는지 확인합니다.
			AppendEditText(pDlg->m_packet_log_edit, stateLog); // Edit Control 끝에 로그 문자열을 추가하는 함수입니다.

		if (hasFrame)
		{
			if (isAckOnly)
			{
				if (g_verboseLog)
				{
					CString ackSendLog; // 화면 출력에 사용할 문자열 변수입니다.
					ackSendLog.Format(_T("[ACK SEND] ack=%d\r\n"), frame.ack_num); // 화면에 표시할 로그 문자열을 구성합니다.
					AppendEditText(pDlg->m_packet_log_edit, ackSendLog); // Edit Control 끝에 로그 문자열을 추가하는 함수입니다.
				}
			}
			else if (isRetransmission) // 앞 조건이 아니면 다음 조건을 검사합니다.
			{
				CString retryLog; // 화면 출력에 사용할 문자열 변수입니다.
				retryLog.Format(_T("⟳ TIMEOUT seq=%d 재전송 %d/%d\r\n"), frame.seq_num, currentRetryCount, STOP_WAIT_MAX_RETRY); // 같은 Frame을 재전송할 최대 횟수입니다.
				AppendEditText(pDlg->m_packet_log_edit, retryLog); // Edit Control 끝에 로그 문자열을 추가하는 함수입니다.
			}
			else // 앞 조건에 해당하지 않는 경우를 처리합니다.
			{
				if (g_verboseLog)
				{
					CString waitLog; // 화면 출력에 사용할 문자열 변수입니다.
					waitLog.Format(_T("[SW SEND] seq=%d ack=%d\r\n[SW WAIT] seq=%d timeout=%dms\r\n"), frame.seq_num, frame.ack_num, frame.seq_num, STOP_WAIT_TIMEOUT_MS); // Stop-and-Wait에서 ACK를 기다릴 timeout 시간입니다.
					AppendEditText(pDlg->m_packet_log_edit, waitLog); // Edit Control 끝에 로그 문자열을 추가하는 함수입니다.
				}
			}

			if (pDlg->m_hSocket != INVALID_SOCKET)
			{
				BYTE nField0, nField1, nField2, nField3; // payload Byte 데이터를 담을 버퍼입니다.
				CString addr; // 화면 출력에 사용할 문자열 변수입니다.
				pDlg->m_ipaddr.GetAddress(nField0, nField1, nField2, nField3);
				addr.Format(_T("%d.%d.%d.%d"), nField0, nField1, nField2, nField3); // 화면에 표시할 로그 문자열을 구성합니다.
				SOCKADDR_IN server_addr;
				memset(&server_addr, 0, sizeof(server_addr)); // 버퍼를 0으로 초기화합니다.
				server_addr.sin_family = AF_INET;
				server_addr.sin_port = htons(8000);
				InetPton(AF_INET, addr, &server_addr.sin_addr);
				BOOL dropThisFrame = FALSE; // 처리 상태를 나타내는 boolean 변수입니다.
				if (!isAckOnly && pDlg->m_injectArmed)
				{
					pDlg->m_injectArmed = FALSE; // 오류 주입 예약이 없도록 초기화합니다.
					dropThisFrame = InjectFrameError(frame, pDlg->m_injectErrorType, pDlg->m_packet_log_edit); // 선택한 오류를 다음 송신 Frame에 1회 주입합니다.
				}
				if (dropThisFrame)
				{
					AppendEditText(pDlg->m_packet_log_edit, _T("[DROP] 이 Frame은 전송되지 않았습니다(오류 주입)\r\n")); // Edit Control 끝에 로그 문자열을 추가하는 함수입니다.
				}
				else // 앞 조건에 해당하지 않는 경우를 처리합니다.
				{
					int sentBytes = sendto(pDlg->m_hSocket, (char*)&frame, sizeof(Frame), 0, (SOCKADDR*)&server_addr, sizeof(server_addr)); // Frame 구조체 전체를 UDP 패킷으로 전송합니다.
					if (sentBytes != SOCKET_ERROR)
						AppendPacketLog(pDlg->m_packet_log_edit, _T("SEND"), frame, sentBytes); // Frame header와 payload 요약 정보를 로그에 남깁니다.
					else // 앞 조건에 해당하지 않는 경우를 처리합니다.
						AppendEditText(pDlg->m_packet_log_edit, _T("[SEND FAIL] sendto failed\r\n")); // Edit Control 끝에 로그 문자열을 추가하는 함수입니다.
				}
			}
		}

		Sleep(10); // busy waiting을 줄이기 위해 잠시 대기합니다.
	}

	return 0;
}

UINT RXThread(LPVOID arg) // UDP로 받은 Frame을 처리하고 완성된 메시지를 출력하는 스레드입니다.
{
	ThreadArg* pArg = (ThreadArg*)arg; // 스레드 인자를 ThreadArg 포인터로 변환합니다.
	CList<Frame, Frame&>* plist = pArg->pList; // Frame들을 임시로 저장할 MFC 리스트입니다.
	CUDPClientThdDlg* pDlg = (CUDPClientThdDlg*)pArg->pDlg;

	while (pArg->Thread_run) // 스레드 실행 여부를 나타내는 플래그입니다.
	{
		pDlg->ProcessReceive(); // UDP Frame을 수신한 뒤 크기, checksum, seq/ACK, reassembly 순서로 검증합니다.

		while (TRUE) // 조건이 만족되는 동안 반복합니다.
		{
			Frame frame; // 처리할 Frame을 임시로 담는 변수입니다.
			BOOL hasFrame = FALSE; // 처리 상태를 나타내는 boolean 변수입니다.

			rx_cs.Lock(); // 공유 자료구조 접근을 보호하기 위해 lock을 잡습니다.
			if (!plist->IsEmpty()) // 처리할 데이터가 없는지 확인합니다.
			{
				frame = plist->RemoveHead(); // 처리한 항목을 리스트에서 제거합니다.
				hasFrame = TRUE;
			}
			rx_cs.Unlock(); // 공유 자료구조 접근이 끝나 lock을 해제합니다.

			if (!hasFrame)
				break; // 현재 반복 또는 분기 처리를 중단합니다.

			AppendPacketLog(pDlg->m_packet_log_edit, _T("RECV"), frame, sizeof(Frame)); // Frame header와 payload 요약 정보를 로그에 남깁니다.
			CString completedText; // 화면 출력에 사용할 문자열 변수입니다.
			if (ProcessReassemblyFrame(pDlg->m_reassemblyList, pDlg->m_packet_log_edit, frame, completedText)) // 수신 fragment를 저장하고 모두 모이면 원본 메시지를 복원합니다.
			{
				completedText += _T("\r\n");
				AppendEditText(pDlg->m_rx_edit, completedText); // Edit Control 끝에 로그 문자열을 추가하는 함수입니다.
			}
		}

		Sleep(10); // busy waiting을 줄이기 위해 잠시 대기합니다.
	}

	return 0;
}


UINT TimerThread(LPVOID arg) // ACK 대기 시간을 누적해 timeout 발생을 TXThread에 알리는 스레드입니다.
{
	ThreadArg* pArg = (ThreadArg*)arg; // 스레드 인자를 ThreadArg 포인터로 변환합니다.
	CUDPClientThdDlg* pDlg = (CUDPClientThdDlg*)pArg->pDlg;

	while (pArg->Thread_run) // 스레드 실행 여부를 나타내는 플래그입니다.
	{
		tx_cs.Lock(); // 공유 자료구조 접근을 보호하기 위해 lock을 잡습니다.
		if (pDlg->m_waitingAck) // 현재 Stop-and-Wait ACK 대기 상태인지 확인합니다.
		{
			pDlg->m_elapsedTimeoutMs += TIMER_INTERVAL_MS; // TimerThread가 ACK 대기 시간을 누적하는 간격입니다.
			if (pDlg->m_elapsedTimeoutMs >= STOP_WAIT_TIMEOUT_MS && !pDlg->m_timeoutFired) // Stop-and-Wait에서 ACK를 기다릴 timeout 시간입니다.
				pDlg->m_timeoutFired = TRUE; // TXThread가 재전송하도록 timeout 플래그를 세웁니다.
		}
		else // 앞 조건에 해당하지 않는 경우를 처리합니다.
		{
			pDlg->m_elapsedTimeoutMs = 0; // ACK 대기 시간을 처음부터 다시 측정합니다.
			pDlg->m_timeoutFired = FALSE; // timeout 플래그를 초기 상태로 돌립니다.
		}
		tx_cs.Unlock(); // 공유 자료구조 접근이 끝나 lock을 해제합니다.

		Sleep(TIMER_INTERVAL_MS); // TimerThread가 ACK 대기 시간을 누적하는 간격입니다.
	}

	return 0;
}




class CAboutDlg : public CDialogEx
{
public:
	CAboutDlg();


#ifdef AFX_DESIGN_TIME
	enum { IDD = IDD_ABOUTBOX };
#endif

	protected:
	virtual void DoDataExchange(CDataExchange* pDX);


protected:
	DECLARE_MESSAGE_MAP()
};

CAboutDlg::CAboutDlg() : CDialogEx(IDD_ABOUTBOX)
{
}

void CAboutDlg::DoDataExchange(CDataExchange* pDX)
{
	CDialogEx::DoDataExchange(pDX);
}

BEGIN_MESSAGE_MAP(CAboutDlg, CDialogEx)
END_MESSAGE_MAP()






CUDPClientThdDlg::CUDPClientThdDlg(CWnd* pParent /*=nullptr*/)
	: CDialogEx(IDD_UDPCLIENTTHD_DIALOG, pParent)
{
	m_hIcon = AfxGetApp()->LoadIcon(IDR_MAINFRAME);
	m_hSocket = INVALID_SOCKET; // UDP 소켓이 아직 생성되지 않았음을 표시합니다.
	m_nextMessageId = 1; // 첫 원본 메시지 번호를 1로 시작합니다.
	m_nextSeqNum = 1; // 첫 송신 Frame의 seq 번호를 1로 시작합니다.
	m_expectedSeqNum = 1; // 처음 수신할 상대 Frame의 seq 번호를 1로 기대합니다.
	m_lastAckNum = 0; // 아직 정상 수신한 상대 Frame이 없음을 ACK 0으로 표시합니다.
	m_lastReceivedAckNum = 0; // 아직 상대가 보낸 ACK를 처리하지 않았음을 표시합니다.
	m_waitingAck = FALSE; // ACK 대기 상태를 해제합니다.
	m_waitFrame = Frame(); // timeout 재전송에 사용할 대기 Frame을 비워 둡니다.
	m_waitAckNum = 0; // 현재 기다리는 ACK 번호가 없음을 표시합니다.
	m_retryCount = 0; // 현재 Frame의 재전송 횟수를 0으로 시작합니다.
	m_lastSendTick = 0; // 마지막 송신 시각을 초기화합니다.
	m_injectErrorType = 0; // 오류 주입 기본값을 정상 모드로 둡니다.
	m_injectArmed = FALSE; // 오류 주입 예약이 없도록 초기화합니다.
	m_elapsedTimeoutMs = 0; // ACK 대기 시간을 처음부터 다시 측정합니다.
	m_timeoutFired = FALSE; // timeout 플래그를 초기 상태로 돌립니다.
}

void CUDPClientThdDlg::DoDataExchange(CDataExchange* pDX)
{
	CDialogEx::DoDataExchange(pDX);
	DDX_Control(pDX, IDC_IPADDRESS, m_ipaddr); // 서버 IP 입력 컨트롤을 멤버 변수와 연결합니다.
	DDX_Control(pDX, IDC_EDIT1, m_tx_edit_short); // 송신 메시지 입력창을 멤버 변수와 연결합니다.
	DDX_Control(pDX, IDC_EDIT2, m_rx_edit); // 수신 메시지 출력창을 멤버 변수와 연결합니다.
	DDX_Control(pDX, IDC_EDIT3, m_tx_edit); // 송신 메시지 출력창을 멤버 변수와 연결합니다.
	DDX_Control(pDX, IDC_PACKET_LOG, m_packet_log_edit); // 패킷 로그 출력창을 멤버 변수와 연결합니다.
	DDX_Control(pDX, IDC_ERROR_TYPE, m_errorTypeCombo); // 오류 종류 combo box를 멤버 변수와 연결합니다.
	DDX_Control(pDX, IDC_VERBOSE_LOG, m_verboseLog); // 상세 로그 checkbox를 멤버 변수와 연결합니다.
}

BEGIN_MESSAGE_MAP(CUDPClientThdDlg, CDialogEx)
	ON_WM_SYSCOMMAND()
	ON_WM_PAINT()
	ON_WM_QUERYDRAGICON()
	ON_BN_CLICKED(IDC_SEND, &CUDPClientThdDlg::OnBnClickedSend) // UI 이벤트와 처리 함수를 연결합니다.
	ON_BN_CLICKED(IDC_CLOSE, &CUDPClientThdDlg::OnBnClickedClose) // UI 이벤트와 처리 함수를 연결합니다.
	ON_BN_CLICKED(IDC_CORRUPT_NEXT, &CUDPClientThdDlg::OnBnClickedCorruptNext) // UI 이벤트와 처리 함수를 연결합니다.
	ON_BN_CLICKED(IDC_VERBOSE_LOG, &CUDPClientThdDlg::OnBnClickedVerboseLog) // UI 이벤트와 처리 함수를 연결합니다.
	ON_EN_CHANGE(IDC_EDIT1, &CUDPClientThdDlg::OnEnChangeEdit1) // UI 이벤트와 처리 함수를 연결합니다.
END_MESSAGE_MAP()




BOOL CUDPClientThdDlg::OnInitDialog()
{
	CDialogEx::OnInitDialog();




	ASSERT((IDM_ABOUTBOX & 0xFFF0) == IDM_ABOUTBOX);
	ASSERT(IDM_ABOUTBOX < 0xF000);

	CMenu* pSysMenu = GetSystemMenu(FALSE);
	if (pSysMenu != nullptr)
	{
		BOOL bNameValid;
		CString strAboutMenu;
		bNameValid = strAboutMenu.LoadString(IDS_ABOUTBOX);
		ASSERT(bNameValid);
		if (!strAboutMenu.IsEmpty())
		{
			pSysMenu->AppendMenu(MF_SEPARATOR);
			pSysMenu->AppendMenu(MF_STRING, IDM_ABOUTBOX, strAboutMenu);
		}
	}



	SetIcon(m_hIcon, TRUE);
	SetIcon(m_hIcon, FALSE);

	m_tx_edit_short.SetLimitText(MAX_MESSAGE_BYTES); // 입력창에서 최대 256Byte 메시지만 입력되도록 제한합니다.

	CList<Frame, Frame&>* newlist = new CList<Frame, Frame&>; // Frame들을 임시로 저장할 MFC 리스트입니다.
	arg1.pList = newlist; // 스레드가 사용할 Frame 큐를 지정합니다.
	arg1.Thread_run = 1; // 스레드 실행 여부를 나타내는 플래그입니다.
	arg1.pDlg = this; // 스레드에서 현재 대화상자 객체에 접근하도록 합니다.

	CList<Frame, Frame&>* newlist2 = new CList<Frame, Frame&>; // Frame들을 임시로 저장할 MFC 리스트입니다.
	arg2.pList = newlist2; // 스레드가 사용할 Frame 큐를 지정합니다.
	arg2.Thread_run = 1; // 스레드 실행 여부를 나타내는 플래그입니다.
	arg2.pDlg = this; // 스레드에서 현재 대화상자 객체에 접근하도록 합니다.

	arg3.pList = NULL; // 스레드가 사용할 Frame 큐를 지정합니다.
	arg3.Thread_run = 1; // 스레드 실행 여부를 나타내는 플래그입니다.
	arg3.pDlg = this; // 스레드에서 현재 대화상자 객체에 접근하도록 합니다.

	m_errorTypeCombo.AddString(_T("정상 (오류 없음)")); // 데모에서 선택할 오류 종류를 combo box에 추가합니다.
	m_errorTypeCombo.AddString(_T("Checksum 오류")); // 데모에서 선택할 오류 종류를 combo box에 추가합니다.
	m_errorTypeCombo.AddString(_T("Seq 번호 오류")); // 데모에서 선택할 오류 종류를 combo box에 추가합니다.
	m_errorTypeCombo.AddString(_T("ACK 번호 오류")); // 데모에서 선택할 오류 종류를 combo box에 추가합니다.
	m_errorTypeCombo.AddString(_T("Payload 길이 오류")); // 데모에서 선택할 오류 종류를 combo box에 추가합니다.
	m_errorTypeCombo.AddString(_T("세그먼트 헤더 오류")); // 데모에서 선택할 오류 종류를 combo box에 추가합니다.
	m_errorTypeCombo.AddString(_T("재조립 미완성")); // 데모에서 선택할 오류 종류를 combo box에 추가합니다.
	m_errorTypeCombo.AddString(_T("패킷 손실")); // 데모에서 선택할 오류 종류를 combo box에 추가합니다.
	m_errorTypeCombo.SetCurSel(0); // combo box의 기본 선택 항목을 지정합니다.
	m_verboseLog.SetCheck(BST_UNCHECKED); // 체크박스의 초기 상태를 지정합니다.
	g_verboseLog = FALSE; // 시작 시 요약 로그 모드로 둡니다.

	m_ipaddr.SetAddress(127, 0, 0, 1); // 기본 서버 IP 주소를 설정합니다.

	m_hSocket = socket(AF_INET, SOCK_DGRAM, 0); // UDP 소켓을 생성합니다.

	if (m_hSocket != INVALID_SOCKET)
	{
		pThread1 = AfxBeginThread(TXThread, (LPVOID)&arg1); // 송신 큐에서 Frame을 꺼내 보내고 timeout 시 재전송하는 스레드입니다.
		pThread2 = AfxBeginThread(RXThread, (LPVOID)&arg2); // UDP로 받은 Frame을 처리하고 완성된 메시지를 출력하는 스레드입니다.
		pThread3 = AfxBeginThread(TimerThread, (LPVOID)&arg3); // ACK 대기 시간을 누적해 timeout 발생을 TXThread에 알리는 스레드입니다.
		return TRUE;
	}

	AfxMessageBox(_T("UDP 클라이언트 소켓 생성 실패")); // UDP 소켓 생성 실패를 사용자에게 알립니다.

	return TRUE;
}

void CUDPClientThdDlg::OnSysCommand(UINT nID, LPARAM lParam)
{
	if ((nID & 0xFFF0) == IDM_ABOUTBOX)
	{
		CAboutDlg dlgAbout;
		dlgAbout.DoModal();
	}
	else
	{
		CDialogEx::OnSysCommand(nID, lParam);
	}
}





void CUDPClientThdDlg::OnPaint()
{
	if (IsIconic())
	{
		CPaintDC dc(this);

		SendMessage(WM_ICONERASEBKGND, reinterpret_cast<WPARAM>(dc.GetSafeHdc()), 0);


		int cxIcon = GetSystemMetrics(SM_CXICON);
		int cyIcon = GetSystemMetrics(SM_CYICON);
		CRect rect;
		GetClientRect(&rect);
		int x = (rect.Width() - cxIcon + 1) / 2;
		int y = (rect.Height() - cyIcon + 1) / 2;


		dc.DrawIcon(x, y, m_hIcon);
	}
	else
	{
		CDialogEx::OnPaint();
	}
}



HCURSOR CUDPClientThdDlg::OnQueryDragIcon()
{
	return static_cast<HCURSOR>(m_hIcon);
}

void CUDPClientThdDlg::ProcessReceive() // UDP Frame을 수신한 뒤 크기, checksum, seq/ACK, reassembly 순서로 검증합니다.
{
	Frame frame; // 처리할 Frame을 임시로 담는 변수입니다.
	int nbytes;

	if (m_hSocket == INVALID_SOCKET)
		return;

	SOCKADDR_IN peer_addr;
	int peer_len = sizeof(peer_addr); // 계산 또는 상태 추적에 사용할 정수 변수입니다.
	nbytes = recvfrom(m_hSocket, (char*)&frame, sizeof(Frame), 0, (SOCKADDR*)&peer_addr, &peer_len); // UDP 소켓에서 Frame 크기만큼 데이터를 수신합니다.

	if (nbytes <= 0)
		return;

	if (nbytes != sizeof(Frame))
	{
		AppendEditText(m_packet_log_edit, _T("[RECV DROP] invalid packet size\r\n")); // Edit Control 끝에 로그 문자열을 추가하는 함수입니다.
		return;
	}

	BOOL ackOnlyFrame = IsAckOnlyFrame(frame); // payload 없이 ACK만 전달하는 제어 Frame인지 확인합니다.
	if (!ackOnlyFrame && (frame.payload_len <= 0 || frame.payload_len > FRAME_PAYLOAD_SIZE)) // 과제 조건에 맞춰 Frame payload를 16Byte로 고정합니다.
	{
		AppendEditText(m_packet_log_edit, _T("[RECV DROP] invalid payload length\r\n")); // Edit Control 끝에 로그 문자열을 추가하는 함수입니다.
		return;
	}

	if (!ackOnlyFrame && !IsValidSegmentHeader(frame)) // fragment header 값이 정상 범위인지 확인합니다.
	{
		AppendEditText(m_packet_log_edit, _T("[RECV DROP] invalid segment header\r\n")); // Edit Control 끝에 로그 문자열을 추가하는 함수입니다.
		return;
	}

	int calculatedChecksum = 0; // 계산 또는 상태 추적에 사용할 정수 변수입니다.
	if (!VerifyFrameChecksum(frame, calculatedChecksum)) // 수신 checksum과 재계산 checksum이 같은지 검증합니다.
	{
		AppendChecksumLog(m_packet_log_edit, _T("FAIL"), frame, calculatedChecksum); // checksum 검증 결과를 패킷 로그창에 남깁니다.
		return;
	}

	AppendChecksumLog(m_packet_log_edit, _T("OK"), frame, calculatedChecksum); // checksum 검증 결과를 패킷 로그창에 남깁니다.

	if (ackOnlyFrame) // ACK-only Frame은 데이터 재조립 없이 ACK 처리만 수행합니다.
	{
		AppendPacketLog(m_packet_log_edit, _T("RECV"), frame, nbytes); // Frame header와 payload 요약 정보를 로그에 남깁니다.
		ProcessStopWaitAck(m_packet_log_edit, frame.ack_num, frame.seq_num, TRUE, m_lastReceivedAckNum, m_nextSeqNum - 1, m_waitingAck, m_waitFrame, m_waitAckNum, m_retryCount, m_lastSendTick); // 수신 ACK로 Stop-and-Wait 대기 상태를 갱신합니다.
		return;
	}

	ApplyXorCipher(frame); // payload에 XOR 변환을 적용하며 송신 시 암호화, 수신 시 복호화에 같이 사용합니다.
	AppendXorCipherLog(m_packet_log_edit, _T("DECRYPT"), frame); // XOR 변환 수행 결과를 상세 로그에 남깁니다.

	if (!ProcessSeqAck(m_packet_log_edit, frame, m_expectedSeqNum, m_lastAckNum, m_lastReceivedAckNum, m_nextSeqNum - 1, m_waitingAck, m_waitFrame, m_waitAckNum, m_retryCount, m_lastSendTick)) // seq 번호와 piggyback ACK를 검증합니다.
	{
		QueueAckOnlyFrame(arg1.pList, m_lastAckNum, m_packet_log_edit); // ACK-only Frame을 데이터보다 먼저 보내도록 송신 큐 앞에 넣습니다.
		return;
	}

	QueueAckOnlyFrame(arg1.pList, m_lastAckNum, m_packet_log_edit); // ACK-only Frame을 데이터보다 먼저 보내도록 송신 큐 앞에 넣습니다.

	rx_cs.Lock(); // 공유 자료구조 접근을 보호하기 위해 lock을 잡습니다.
	arg2.pList->AddTail(frame); // 리스트 뒤쪽에 새 항목을 추가합니다.
	rx_cs.Unlock(); // 공유 자료구조 접근이 끝나 lock을 해제합니다.
}

void CUDPClientThdDlg::OnBnClickedSend() // Send 버튼 클릭 시 입력 메시지를 Frame으로 나누어 송신 큐에 넣습니다.
{
	CString tx_message; // 화면 출력에 사용할 문자열 변수입니다.
	CList<Frame, Frame&> frameList; // Frame들을 임시로 저장할 MFC 리스트입니다.
	int messageBytes = 0; // 계산 또는 상태 추적에 사용할 정수 변수입니다.
	int messageId = m_nextMessageId; // 계산 또는 상태 추적에 사용할 정수 변수입니다.
	m_tx_edit_short.GetWindowTextW(tx_message); // 입력창의 현재 문자열을 읽습니다.

	if (!BuildFramesFromText(tx_message, frameList, messageId, messageBytes)) // 입력 문자열을 16Byte payload Frame들로 분할합니다.
	{
		AppendEditText(m_packet_log_edit, _T("[SEND SKIP] message must be 1-256 bytes\r\n")); // Edit Control 끝에 로그 문자열을 추가하는 함수입니다.
		m_tx_edit_short.SetFocus(); // 사용자가 바로 입력할 수 있도록 포커스를 옮깁니다.
		return;
	}

	m_nextMessageId++; // 다음 원본 메시지 번호로 증가시킵니다.
	CString tx_log = tx_message + _T("\r\n"); // 화면 출력에 사용할 문자열 변수입니다.
	AppendEditText(m_tx_edit, tx_log); // Edit Control 끝에 로그 문자열을 추가하는 함수입니다.
	AppendSegmentLog(m_packet_log_edit, messageId, messageBytes, (int)frameList.GetCount());
	ApplySeqAckToFrames(frameList, m_nextSeqNum, m_lastAckNum, m_packet_log_edit); // 각 Frame에 seq/ACK를 넣고 XOR 변환 후 checksum을 계산합니다.

	POSITION pos = frameList.GetHeadPosition(); // MFC 리스트 순회를 시작할 위치를 얻습니다.
	while (pos != NULL) // 조건이 만족되는 동안 반복합니다.
	{
		Frame frame = frameList.GetNext(pos); // 현재 위치의 항목을 가져오고 다음 위치로 이동합니다.
		AppendPacketLog(m_packet_log_edit, _T("CREATE"), frame, sizeof(Frame)); // Frame header와 payload 요약 정보를 로그에 남깁니다.
	}

	tx_cs.Lock(); // 공유 자료구조 접근을 보호하기 위해 lock을 잡습니다.
	pos = frameList.GetHeadPosition(); // MFC 리스트 순회를 시작할 위치를 얻습니다.
	while (pos != NULL) // 조건이 만족되는 동안 반복합니다.
	{
		Frame frame = frameList.GetNext(pos); // 현재 위치의 항목을 가져오고 다음 위치로 이동합니다.
		arg1.pList->AddTail(frame); // 리스트 뒤쪽에 새 항목을 추가합니다.
	}
	tx_cs.Unlock(); // 공유 자료구조 접근이 끝나 lock을 해제합니다.

	m_tx_edit_short.SetWindowTextW(_T("")); // 입력창의 문자열을 갱신합니다.
	m_tx_edit_short.SetFocus(); // 사용자가 바로 입력할 수 있도록 포커스를 옮깁니다.
}

void CUDPClientThdDlg::OnBnClickedCorruptNext() // Corrupt 버튼 클릭 시 다음 Frame에 적용할 오류 주입을 예약합니다.
{
	m_injectErrorType = m_errorTypeCombo.GetCurSel(); // combo box에서 선택된 항목 번호를 읽습니다.
	if (m_injectErrorType <= 0)
	{
		m_injectArmed = FALSE; // 오류 주입 예약이 없도록 초기화합니다.
		AppendEditText(m_packet_log_edit, _T("[주입] 정상 모드 - 오류를 주입하지 않습니다\r\n")); // Edit Control 끝에 로그 문자열을 추가하는 함수입니다.
		return;
	}

	m_injectArmed = TRUE;
	CString typeName; // 화면 출력에 사용할 문자열 변수입니다.
	m_errorTypeCombo.GetLBText(m_injectErrorType, typeName); // combo box에서 선택된 항목 이름을 읽습니다.
	CString readyLog; // 화면 출력에 사용할 문자열 변수입니다.
	readyLog.Format(_T("[주입 예약] %s -> 다음 패킷에 1회 적용\r\n"), typeName.GetString()); // 화면에 표시할 로그 문자열을 구성합니다.
	AppendEditText(m_packet_log_edit, readyLog); // Edit Control 끝에 로그 문자열을 추가하는 함수입니다.
}

void CUDPClientThdDlg::OnBnClickedVerboseLog() // 상세 로그와 요약 로그 모드를 전환합니다.
{
	g_verboseLog = (m_verboseLog.GetCheck() == BST_CHECKED); // 체크박스의 현재 상태를 읽습니다.
	if (g_verboseLog)
		AppendEditText(m_packet_log_edit, _T("[로그] 상세 모드로 전환되었습니다\r\n")); // Edit Control 끝에 로그 문자열을 추가하는 함수입니다.
	else // 앞 조건에 해당하지 않는 경우를 처리합니다.
		AppendEditText(m_packet_log_edit, _T("[로그] 요약 모드로 전환되었습니다\r\n")); // Edit Control 끝에 로그 문자열을 추가하는 함수입니다.
}

void CUDPClientThdDlg::OnEnChangeEdit1() // 입력창 내용이 256Byte를 넘지 않도록 제한합니다.
{
	if (g_isTrimmingInput)
		return;

	CString currentText; // 화면 출력에 사용할 문자열 변수입니다.
	CString limitedText; // 화면 출력에 사용할 문자열 변수입니다.
	m_tx_edit_short.GetWindowTextW(currentText); // 입력창의 현재 문자열을 읽습니다.

	if (!LimitTextToMessageSize(currentText, limitedText)) // 입력 문자열을 256Byte 제한 안에 들어가도록 자릅니다.
		return;

	g_isTrimmingInput = TRUE;
	m_tx_edit_short.SetWindowTextW(limitedText); // 입력창의 문자열을 갱신합니다.
	m_tx_edit_short.SetSel(limitedText.GetLength(), limitedText.GetLength()); // Edit Control의 커서 위치를 지정합니다.
	g_isTrimmingInput = FALSE;
}

void CUDPClientThdDlg::OnBnClickedClose() // 스레드를 종료하고 UDP 소켓을 닫습니다.
{
	arg1.Thread_run = 0; // 스레드 실행 여부를 나타내는 플래그입니다.
	arg2.Thread_run = 0; // 스레드 실행 여부를 나타내는 플래그입니다.
	arg3.Thread_run = 0; // 스레드 실행 여부를 나타내는 플래그입니다.

	if (m_hSocket != INVALID_SOCKET)
	{
		closesocket(m_hSocket); // UDP 소켓을 생성합니다.
		m_hSocket = INVALID_SOCKET; // UDP 소켓이 아직 생성되지 않았음을 표시합니다.
	}

	CDialogEx::OnOK();
}

