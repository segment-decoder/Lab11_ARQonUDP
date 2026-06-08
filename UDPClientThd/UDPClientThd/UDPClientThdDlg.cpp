
// UDPClientThdDlg.cpp: 구현 파일
//

#include "pch.h"
#include "framework.h"
#include "UDPClientThd.h"
#include "UDPClientThdDlg.h"
#include "afxdialogex.h"

#pragma comment(lib, "ws2_32.lib")

#ifdef _DEBUG
#define new DEBUG_NEW
#endif

CCriticalSection tx_cs; // 송신 리스트 접근을 보호하는 임계 영역입니다.
CCriticalSection rx_cs; // 수신 리스트 접근을 보호하는 임계 영역입니다.

BOOL g_isTrimmingInput = FALSE; // 입력창을 코드로 수정할 때 EN_CHANGE가 재진입하지 않도록 막습니다.

void AppendEditText(CEdit& edit, const CString& text) // 지정한 Edit Control의 끝에 로그 문자열을 추가합니다.
{
	int len = edit.GetWindowTextLengthW(); // 기존 출력 문자열의 끝 위치를 구합니다.
	edit.SetSel(len, len); // 새 로그가 끝에 붙도록 커서를 이동합니다.
	edit.ReplaceSel(text); // 전달받은 로그 문자열을 화면에 추가합니다.
}

int GetUtf8ByteCount(const CStringW& text) // CStringW가 UTF-8로 바뀔 때 필요한 Byte 수를 계산합니다.
{
	if (text.IsEmpty()) // 빈 문자열은 전송할 Byte가 없습니다.
		return 0;

	return WideCharToMultiByte(CP_UTF8, 0, text, text.GetLength(), NULL, 0, NULL, NULL); // UTF-8 변환 결과의 Byte 수를 반환합니다.
}

void AddChecksumWord(DWORD& sum, WORD word) // 16-bit 단어를 Checksum 누적합에 더하고 carry를 접습니다.
{
	sum += word; // 새 16-bit 단어를 누적합에 더합니다.
	while ((sum >> 16) != 0) // 16-bit 범위를 넘는 carry가 남아 있으면 반복해서 접습니다.
		sum = (sum & 0xFFFF) + (sum >> 16);
}

void AddChecksumInt(DWORD& sum, int value) // 32-bit Header 값을 16-bit 단어 두 개로 나누어 Checksum에 반영합니다.
{
	DWORD unsignedValue = (DWORD)value; // 비트 패턴을 유지한 채 32-bit 값으로 변환합니다.
	AddChecksumWord(sum, (WORD)((unsignedValue >> 16) & 0xFFFF)); // Header 값의 상위 16-bit를 누적합니다.
	AddChecksumWord(sum, (WORD)(unsignedValue & 0xFFFF)); // Header 값의 하위 16-bit를 누적합니다.
}

void AddChecksumPayload(DWORD& sum, const BYTE* payload, int payloadLen) // Payload Byte들을 16-bit 단어 단위로 Checksum에 반영합니다.
{
	for (int index = 0; index < payloadLen; index += 2) // Payload를 앞에서부터 2Byte씩 묶어 처리합니다.
	{
		WORD word = ((WORD)payload[index]) << 8; // 첫 번째 Byte를 16-bit 단어의 상위 Byte로 배치합니다.
		if (index + 1 < payloadLen) // 짝이 되는 두 번째 Byte가 있으면 하위 Byte로 배치합니다.
			word |= payload[index + 1];

		AddChecksumWord(sum, word); // 완성된 16-bit Payload 단어를 누적합니다.
	}
}

int CalculateFrameChecksum(const Frame& frame) // Frame Header와 실제 Payload를 기준으로 16-bit 1의 보수 Checksum을 계산합니다.
{
	DWORD sum = 0; // Checksum 계산에 사용할 32-bit 누적합입니다.
	AddChecksumInt(sum, frame.seq_num); // 순서 번호 Header 값을 Checksum에 반영합니다.
	AddChecksumInt(sum, frame.ack_num); // ACK 번호 Header 값을 Checksum에 반영합니다.
	AddChecksumInt(sum, frame.msg_id); // 메시지 번호 Header 값을 Checksum에 반영합니다.
	AddChecksumInt(sum, frame.frag_index); // 조각 번호 Header 값을 Checksum에 반영합니다.
	AddChecksumInt(sum, frame.frag_count); // 전체 조각 수 Header 값을 Checksum에 반영합니다.
	AddChecksumInt(sum, frame.payload_len); // 실제 Payload 길이 Header 값을 Checksum에 반영합니다.
	AddChecksumPayload(sum, frame.payload, frame.payload_len); // 실제 Payload Byte를 Checksum에 반영합니다.
	while ((sum >> 16) != 0) // 마지막으로 남은 carry가 있으면 16-bit로 접습니다.
		sum = (sum & 0xFFFF) + (sum >> 16);

	return (int)(~sum & 0xFFFF); // 1의 보수를 취해 16-bit Checksum 값으로 반환합니다.
}

BOOL VerifyFrameChecksum(const Frame& frame, int& calculatedChecksum) // 수신 Frame의 Checksum이 재계산 결과와 같은지 확인합니다.
{
	calculatedChecksum = CalculateFrameChecksum(frame); // 수신 Frame을 기준으로 Checksum을 다시 계산합니다.
	return frame.checksum == calculatedChecksum; // Frame에 담긴 Checksum과 재계산 결과가 같은지 반환합니다.
}

void AppendChecksumLog(CEdit& edit, LPCTSTR result, const Frame& frame, int calculatedChecksum) // Checksum 검증 결과를 패킷 로그창에 출력합니다.
{
	CString log; // 화면에 출력할 Checksum 로그 문자열입니다.
	log.Format(_T("[CHECKSUM %s] msg=%d frag=%d/%d recv=%d calc=%d\r\n"), result, frame.msg_id, frame.frag_index + 1, frame.frag_count, frame.checksum, calculatedChecksum); // 수신 Checksum과 재계산 Checksum을 비교할 수 있는 로그를 만듭니다.
	AppendEditText(edit, log); // 지정한 출력창에 Checksum 검증 로그를 추가합니다.
}

BOOL CorruptFrameForChecksumDemo(Frame& frame) // Checksum 실패 시연을 위해 Frame Payload 일부를 고의로 손상합니다.
{
	if (frame.payload_len <= 0) // 손상할 Payload가 없으면 실패로 반환합니다.
		return FALSE;

	frame.payload[0] ^= 0x01; // Checksum은 그대로 둔 채 첫 Payload Byte의 마지막 bit를 뒤집습니다.
	return TRUE; // Frame 손상이 완료되었음을 알립니다.
}

void AppendCorruptPacketLog(CEdit& edit, const Frame& frame) // 고의 손상한 Frame 정보를 패킷 로그창에 출력합니다.
{
	CString log; // 화면에 출력할 고의 손상 로그 문자열입니다.
	log.Format(_T("[CORRUPT PACKET] msg=%d frag=%d/%d payload_byte=0 checksum_kept=%d\r\n"), frame.msg_id, frame.frag_index + 1, frame.frag_count, frame.checksum); // 어떤 Frame을 손상했는지 확인 가능한 로그를 만듭니다.
	AppendEditText(edit, log); // 지정한 출력창에 고의 손상 로그를 추가합니다.
}

CString Utf8BytesToText(const BYTE* payload, int payloadLen, int maxBytes) // UTF-8 Byte 배열을 화면 출력용 CString으로 복원합니다.
{
	if (payloadLen <= 0 || payloadLen > maxBytes) // Byte 길이가 허용 범위를 벗어나면 빈 문자열을 반환합니다.
		return _T("");

	int wideChars = MultiByteToWideChar(CP_UTF8, 0, (LPCCH)payload, payloadLen, NULL, 0); // UTF-8 Byte를 Unicode로 바꿀 때 필요한 문자 수를 계산합니다.
	if (wideChars <= 0) // UTF-8 변환에 실패하면 빈 문자열을 반환합니다.
		return _T("");

	CStringW wideText; // 복원된 Unicode 문자열을 저장합니다.
	LPWSTR buffer = wideText.GetBuffer(wideChars); // MultiByteToWideChar가 쓸 문자열 버퍼를 확보합니다.
	MultiByteToWideChar(CP_UTF8, 0, (LPCCH)payload, payloadLen, buffer, wideChars); // Payload Byte를 Unicode 문자열로 복원합니다.
	wideText.ReleaseBuffer(wideChars); // CStringW 버퍼 길이를 실제 복원 길이로 확정합니다.
	return CString(wideText); // MFC 화면 출력용 CString으로 반환합니다.
}

BOOL LimitTextToMessageSize(const CString& source, CString& limitedText) // 입력 문자열을 전체 메시지 제한 256Byte 안에 들어가도록 자릅니다.
{
	CStringW sourceText(source); // Unicode CString을 UTF-8 Byte 계산용 문자열로 변환합니다.
	CStringW limitedWideText; // 256Byte 안에 들어가는 문자만 누적합니다.
	int usedBytes = 0; // 현재까지 누적된 UTF-8 Byte 수입니다.
	int index = 0; // UTF-16 문자 위치를 추적합니다.

	while (index < sourceText.GetLength()) // 입력 문자열을 앞에서부터 문자 단위로 검사합니다.
	{
		int unitCount = 1; // 기본적으로 UTF-16 코드 유닛 하나를 문자 단위로 봅니다.
		WCHAR currentChar = sourceText[index]; // 현재 검사 중인 UTF-16 코드 유닛입니다.

		if (currentChar >= 0xD800 && currentChar <= 0xDBFF && index + 1 < sourceText.GetLength()) // 서로게이트 쌍의 앞부분인지 확인합니다.
		{
			WCHAR nextChar = sourceText[index + 1]; // 서로게이트 쌍의 뒷부분 후보를 가져옵니다.
			if (nextChar >= 0xDC00 && nextChar <= 0xDFFF) // 올바른 서로게이트 쌍이면 두 코드 유닛을 같이 처리합니다.
				unitCount = 2;
		}

		CStringW unitText = sourceText.Mid(index, unitCount); // 현재 문자 단위를 UTF-8 Byte 계산 대상으로 잘라냅니다.
		int unitBytes = WideCharToMultiByte(CP_UTF8, 0, unitText, unitCount, NULL, 0, NULL, NULL); // 현재 문자 단위의 UTF-8 Byte 수를 계산합니다.
		if (usedBytes + unitBytes > MAX_MESSAGE_BYTES) // 256Byte를 넘으면 더 이상 입력을 받지 않습니다.
			break;

		limitedWideText += unitText; // 제한 안에 들어가는 문자를 결과 문자열에 추가합니다.
		usedBytes += unitBytes; // 누적 Byte 수를 갱신합니다.
		index += unitCount; // 다음 문자 단위로 이동합니다.
	}

	limitedText = CString(limitedWideText); // 제한된 Unicode 문자열을 MFC CString으로 되돌립니다.
	return sourceText.GetLength() != limitedWideText.GetLength(); // 실제로 잘라낸 문자가 있는지 반환합니다.
}

BOOL BuildFramesFromText(const CString& text, CList<Frame, Frame&>& frameList, int messageId, int& messageBytes) // 입력 문자열을 16Byte Payload를 가진 여러 Frame으로 분할합니다.
{
	CStringW wideText(text); // UI 입력 문자열을 UTF-8 변환용 Unicode 문자열로 준비합니다.
	int utf8Bytes = GetUtf8ByteCount(wideText); // 전송 Payload에 들어갈 실제 Byte 수를 계산합니다.
	Frame currentFrame; // 현재 채우고 있는 세그먼트 Frame입니다.
	int index = 0; // UTF-16 문자 위치를 추적합니다.
	int fragmentCount = 0; // 생성된 세그먼트 개수를 저장합니다.
	POSITION pos; // 생성된 Frame 리스트를 다시 순회할 위치 값입니다.
	int fragmentIndex = 0; // 각 Frame에 기록할 조각 번호입니다.

	frameList.RemoveAll(); // 호출자가 넘긴 임시 Frame 리스트를 새 메시지 기준으로 비웁니다.
	messageBytes = utf8Bytes; // 호출자가 로그에 표시할 전체 메시지 Byte 수를 저장합니다.

	if (utf8Bytes <= 0) // 빈 메시지는 Frame으로 만들지 않습니다.
		return FALSE;

	if (utf8Bytes > MAX_MESSAGE_BYTES) // UI 제한을 우회한 256Byte 초과 입력은 송신하지 않습니다.
		return FALSE;

	while (index < wideText.GetLength()) // 입력 문자열을 문자 단위로 읽으면서 Frame Payload를 채웁니다.
	{
		int unitCount = 1; // 기본적으로 UTF-16 코드 유닛 하나를 문자 단위로 봅니다.
		WCHAR currentChar = wideText[index]; // 현재 검사 중인 UTF-16 코드 유닛입니다.

		if (currentChar >= 0xD800 && currentChar <= 0xDBFF && index + 1 < wideText.GetLength()) // 서로게이트 쌍의 앞부분인지 확인합니다.
		{
			WCHAR nextChar = wideText[index + 1]; // 서로게이트 쌍의 뒷부분 후보를 가져옵니다.
			if (nextChar >= 0xDC00 && nextChar <= 0xDFFF) // 올바른 서로게이트 쌍이면 두 코드 유닛을 같이 처리합니다.
				unitCount = 2;
		}

		CStringW unitText = wideText.Mid(index, unitCount); // 현재 문자 단위를 UTF-8 변환 대상으로 잘라냅니다.
		int unitBytes = WideCharToMultiByte(CP_UTF8, 0, unitText, unitCount, NULL, 0, NULL, NULL); // 현재 문자 단위가 차지할 UTF-8 Byte 수를 계산합니다.
		if (unitBytes <= 0 || unitBytes > FRAME_PAYLOAD_SIZE) // 한 문자 단위가 Payload에 들어갈 수 없으면 Frame 생성을 중단합니다.
			return FALSE;

		if (currentFrame.payload_len + unitBytes > FRAME_PAYLOAD_SIZE) // 현재 Frame에 더 담을 수 없으면 새 Frame을 시작합니다.
		{
			frameList.AddTail(currentFrame); // 가득 찬 Frame을 임시 리스트에 추가합니다.
			fragmentCount++; // 생성된 세그먼트 수를 증가시킵니다.
			currentFrame = Frame(); // 다음 세그먼트를 담을 빈 Frame으로 초기화합니다.
		}

		WideCharToMultiByte(CP_UTF8, 0, unitText, unitCount, (LPSTR)(currentFrame.payload + currentFrame.payload_len), FRAME_PAYLOAD_SIZE - currentFrame.payload_len, NULL, NULL); // 현재 문자 단위를 Frame Payload 뒤쪽에 이어 붙입니다.
		currentFrame.payload_len += unitBytes; // 현재 Frame의 실제 Payload Byte 수를 갱신합니다.
		index += unitCount; // 다음 문자 단위로 이동합니다.
	}

	if (currentFrame.payload_len > 0) // 마지막 Frame에 남은 Payload가 있으면 리스트에 추가합니다.
	{
		frameList.AddTail(currentFrame); // 마지막 세그먼트 Frame을 임시 리스트에 추가합니다.
		fragmentCount++; // 생성된 세그먼트 수를 증가시킵니다.
	}

	pos = frameList.GetHeadPosition(); // 생성된 Frame들의 Header를 채우기 위해 리스트 처음 위치를 가져옵니다.
	while (pos != NULL) // 모든 Frame에 동일한 메시지 번호와 조각 정보를 기록합니다.
	{
		Frame& segmentFrame = frameList.GetNext(pos); // 현재 조각 Frame을 참조로 가져옵니다.
		segmentFrame.msg_id = messageId; // 원본 메시지를 구분할 메시지 번호를 저장합니다.
		segmentFrame.frag_index = fragmentIndex; // 현재 조각의 0부터 시작하는 번호를 저장합니다.
		segmentFrame.frag_count = fragmentCount; // 전체 조각 개수를 저장합니다.
		segmentFrame.checksum = CalculateFrameChecksum(segmentFrame); // 완성된 Header와 Payload를 기준으로 Checksum 값을 계산해 저장합니다.
		fragmentIndex++; // 다음 조각 번호로 이동합니다.
	}

	return !frameList.IsEmpty(); // 하나 이상의 Frame이 만들어졌는지 반환합니다.
}

CString FramePayloadToText(const Frame& frame) // 수신한 Frame Payload를 화면 출력용 CString으로 복원합니다.
{
	if (frame.payload_len <= 0 || frame.payload_len > FRAME_PAYLOAD_SIZE) // Payload 길이가 올바르지 않으면 빈 문자열을 반환합니다.
		return _T("");

	return Utf8BytesToText(frame.payload, frame.payload_len, FRAME_PAYLOAD_SIZE); // Frame 하나의 Payload Byte를 CString으로 복원합니다.
}

void AppendPacketLog(CEdit& edit, LPCTSTR action, const Frame& frame, int packetBytes) // Frame 송수신 결과를 검증 가능한 로그로 출력합니다.
{
	CString payloadText = FramePayloadToText(frame); // 로그에 보여줄 Payload 문자열을 복원합니다.
	CString log; // 화면에 출력할 패킷 로그 문자열입니다.
	log.Format(_T("[%s PACKET] packet_bytes=%d seq=%d ack=%d checksum=%d msg=%d frag=%d/%d payload_len=%d payload=\"%s\"\r\n"), action, packetBytes, frame.seq_num, frame.ack_num, frame.checksum, frame.msg_id, frame.frag_index + 1, frame.frag_count, frame.payload_len, payloadText.GetString()); // Header 값과 세그먼트 정보를 포함한 로그를 만듭니다.
	AppendEditText(edit, log); // 지정한 출력창에 패킷 로그를 추가합니다.
}

void AppendSegmentLog(CEdit& edit, int messageId, int messageBytes, int fragmentCount) // 원본 메시지가 몇 개의 Frame으로 분할되었는지 로그로 출력합니다.
{
	CString log; // 화면에 출력할 세그먼트 로그 문자열입니다.
	log.Format(_T("[SEGMENT] msg=%d message_bytes=%d fragments=%d payload_limit=%d\r\n"), messageId, messageBytes, fragmentCount, FRAME_PAYLOAD_SIZE); // 메시지 번호, 전체 Byte 수, 조각 수를 포함한 로그를 만듭니다.
	AppendEditText(edit, log); // 지정한 출력창에 세그먼트 로그를 추가합니다.
}

BOOL IsValidSegmentHeader(const Frame& frame) // 수신한 Frame의 세그먼트 Header 값이 정상 범위인지 확인합니다.
{
	if (frame.msg_id <= 0) // 메시지 번호는 1 이상이어야 합니다.
		return FALSE;

	if (frame.frag_count <= 0) // 전체 조각 수는 1 이상이어야 합니다.
		return FALSE;

	if (frame.frag_count > MAX_SEGMENT_COUNT) // 전체 조각 수는 256Byte 메시지가 만들 수 있는 최대 조각 수를 넘을 수 없습니다.
		return FALSE;

	if (frame.frag_index < 0) // 조각 번호는 음수가 될 수 없습니다.
		return FALSE;

	if (frame.frag_index >= frame.frag_count) // 조각 번호는 전체 조각 수보다 작아야 합니다.
		return FALSE;

	return TRUE; // 세그먼트 Header 값이 모두 정상임을 알립니다.
}

POSITION FindReassemblyMessage(CList<ReassemblyMessage, ReassemblyMessage&>& reassemblyList, int messageId) // 메시지 번호에 해당하는 재조립 버퍼 위치를 찾습니다.
{
	POSITION pos = reassemblyList.GetHeadPosition(); // 재조립 리스트의 첫 위치를 가져옵니다.
	while (pos != NULL) // 모든 재조립 버퍼를 앞에서부터 검사합니다.
	{
		POSITION currentPos = pos; // 현재 버퍼 위치를 반환할 수 있도록 따로 저장합니다.
		ReassemblyMessage& message = reassemblyList.GetNext(pos); // 현재 재조립 버퍼를 가져옵니다.
		if (message.msg_id == messageId) // 찾는 메시지 번호와 같으면 현재 위치를 반환합니다.
			return currentPos;
	}

	return NULL; // 같은 메시지 번호를 가진 재조립 버퍼가 없음을 알립니다.
}

void AppendReassemblyDropLog(CEdit& edit, int messageId, LPCTSTR reason) // 재조립할 수 없는 조각이나 미완성 메시지 폐기 사유를 로그로 출력합니다.
{
	CString log; // 화면에 출력할 재조립 폐기 로그 문자열입니다.
	log.Format(_T("[REASSEMBLY DROP] msg=%d reason=%s\r\n"), messageId, reason); // 메시지 번호와 폐기 사유를 포함한 로그를 만듭니다.
	AppendEditText(edit, log); // 지정한 출력창에 재조립 폐기 로그를 추가합니다.
}

void CleanupExpiredReassemblyMessages(CList<ReassemblyMessage, ReassemblyMessage&>& reassemblyList, CEdit& edit) // 오래된 미완성 재조립 메시지를 정리합니다.
{
	DWORD nowTick = GetTickCount(); // 현재 시간을 밀리초 단위로 가져옵니다.
	POSITION pos = reassemblyList.GetHeadPosition(); // 재조립 리스트의 첫 위치를 가져옵니다.

	while (pos != NULL) // 재조립 중인 모든 메시지를 검사합니다.
	{
		POSITION removePos = pos; // 삭제할 수 있도록 현재 위치를 따로 저장합니다.
		ReassemblyMessage& message = reassemblyList.GetNext(pos); // 현재 재조립 버퍼를 가져옵니다.
		if (nowTick - message.last_update_tick > REASSEMBLY_TIMEOUT_MS) // 마지막 조각 수신 후 정리 시간이 지났는지 확인합니다.
		{
			AppendReassemblyDropLog(edit, message.msg_id, _T("incomplete timeout")); // 미완성 메시지가 시간 초과로 정리됨을 로그에 남깁니다.
			reassemblyList.RemoveAt(removePos); // 오래된 재조립 버퍼를 리스트에서 제거합니다.
		}
	}
}

BOOL BuildReassembledText(const ReassemblyMessage& message, CString& completedText) // 모든 조각의 Payload를 순서대로 합쳐 원본 문자열로 복원합니다.
{
	BYTE payload[MAX_MESSAGE_BYTES]; // 재조립된 UTF-8 Byte를 담을 임시 버퍼입니다.
	int offset = 0; // 임시 버퍼에 다음 Payload를 붙일 위치입니다.

	memset(payload, 0, sizeof(payload)); // 재조립 임시 버퍼를 0으로 초기화합니다.
	completedText.Empty(); // 이전 결과 문자열이 남지 않도록 비웁니다.

	for (int index = 0; index < message.frag_count; index++) // 조각 번호 순서대로 모든 Payload를 이어 붙입니다.
	{
		if (!message.received[index]) // 중간에 빠진 조각이 있으면 재조립을 실패 처리합니다.
			return FALSE;

		const Frame& frame = message.fragments[index]; // 현재 순서의 Frame 조각을 가져옵니다.
		if (offset + frame.payload_len > MAX_MESSAGE_BYTES) // 재조립 결과가 전체 메시지 제한을 넘으면 실패 처리합니다.
			return FALSE;

		memcpy(payload + offset, frame.payload, frame.payload_len); // 현재 조각의 Payload를 재조립 버퍼 뒤에 붙입니다.
		offset += frame.payload_len; // 다음 조각을 붙일 위치를 갱신합니다.
	}

	completedText = Utf8BytesToText(payload, offset, MAX_MESSAGE_BYTES); // 재조립된 UTF-8 Byte를 화면 출력용 문자열로 복원합니다.
	return !completedText.IsEmpty(); // 복원된 문자열이 있으면 재조립 성공으로 판단합니다.
}

BOOL ProcessReassemblyFrame(CList<ReassemblyMessage, ReassemblyMessage&>& reassemblyList, CEdit& edit, const Frame& frame, CString& completedText) // 수신 Frame 조각을 저장하고 완성되면 원본 메시지를 반환합니다.
{
	CleanupExpiredReassemblyMessages(reassemblyList, edit); // 새 조각을 처리하기 전에 오래된 미완성 메시지를 정리합니다.
	completedText.Empty(); // 호출자에게 넘길 재조립 완료 문자열을 비웁니다.

	POSITION messagePos = FindReassemblyMessage(reassemblyList, frame.msg_id); // 같은 메시지 번호로 재조립 중인 버퍼가 있는지 찾습니다.
	if (messagePos == NULL) // 처음 도착한 메시지 번호이면 새 재조립 버퍼를 만듭니다.
	{
		ReassemblyMessage newMessage; // 새 원본 메시지를 모을 재조립 버퍼입니다.
		newMessage.msg_id = frame.msg_id; // 수신 Frame의 메시지 번호를 저장합니다.
		newMessage.frag_count = frame.frag_count; // 수신 Frame의 전체 조각 수를 저장합니다.
		newMessage.last_update_tick = GetTickCount(); // 재조립 버퍼의 최근 갱신 시간을 기록합니다.
		messagePos = reassemblyList.AddTail(newMessage); // 새 재조립 버퍼를 리스트 끝에 추가합니다.
	}

	ReassemblyMessage& message = reassemblyList.GetAt(messagePos); // 현재 Frame이 들어갈 재조립 버퍼를 가져옵니다.
	if (message.frag_count != frame.frag_count) // 같은 메시지 번호에서 전체 조각 수가 달라지면 비정상 조각으로 판단합니다.
	{
		AppendReassemblyDropLog(edit, frame.msg_id, _T("fragment count mismatch")); // 조각 수 불일치 사유를 로그에 남깁니다.
		return FALSE;
	}

	if (message.received[frame.frag_index]) // 이미 받은 조각 번호이면 중복 조각으로 판단합니다.
	{
		AppendReassemblyDropLog(edit, frame.msg_id, _T("duplicate fragment")); // 중복 조각 폐기 사유를 로그에 남깁니다.
		return FALSE;
	}

	message.fragments[frame.frag_index] = frame; // 현재 조각을 조각 번호 위치에 저장합니다.
	message.received[frame.frag_index] = TRUE; // 현재 조각 번호를 수신 완료로 표시합니다.
	message.received_count++; // 현재 메시지의 수신 조각 수를 증가시킵니다.
	message.total_payload_len += frame.payload_len; // 재조립될 전체 Payload Byte 수를 누적합니다.
	message.last_update_tick = GetTickCount(); // 재조립 버퍼의 최근 갱신 시간을 갱신합니다.

	CString storeLog; // 화면에 출력할 조각 저장 로그 문자열입니다.
	storeLog.Format(_T("[REASSEMBLY STORE] msg=%d frag=%d/%d received=%d/%d\r\n"), frame.msg_id, frame.frag_index + 1, frame.frag_count, message.received_count, message.frag_count); // 저장된 조각과 현재 수신 현황을 로그로 만듭니다.
	AppendEditText(edit, storeLog); // 지정한 출력창에 조각 저장 로그를 추가합니다.

	if (message.received_count < message.frag_count) // 아직 모든 조각이 모이지 않았으면 대기 상태를 출력합니다.
	{
		CString waitLog; // 화면에 출력할 재조립 대기 로그 문자열입니다.
		waitLog.Format(_T("[REASSEMBLY WAIT] msg=%d received=%d/%d\r\n"), message.msg_id, message.received_count, message.frag_count); // 현재까지 받은 조각 수를 로그로 만듭니다.
		AppendEditText(edit, waitLog); // 지정한 출력창에 재조립 대기 로그를 추가합니다.
		return FALSE;
	}

	if (!BuildReassembledText(message, completedText)) // 모든 조각을 합쳐 원본 문자열로 복원합니다.
	{
		AppendReassemblyDropLog(edit, frame.msg_id, _T("rebuild failed")); // 재조립 실패 사유를 로그에 남깁니다.
		reassemblyList.RemoveAt(messagePos); // 실패한 재조립 버퍼를 제거합니다.
		return FALSE;
	}

	CString doneLog; // 화면에 출력할 재조립 완료 로그 문자열입니다.
	doneLog.Format(_T("[REASSEMBLY DONE] msg=%d fragments=%d bytes=%d\r\n"), message.msg_id, message.frag_count, message.total_payload_len); // 재조립 완료 정보를 로그로 만듭니다.
	AppendEditText(edit, doneLog); // 지정한 출력창에 재조립 완료 로그를 추가합니다.
	reassemblyList.RemoveAt(messagePos); // 완료된 재조립 버퍼를 리스트에서 제거합니다.
	return TRUE; // 원본 메시지가 완성되었음을 호출자에게 알립니다.
}

UINT TXThread(LPVOID arg) // 송신 리스트의 메시지를 UDP로 전송하는 스레드 함수입니다.
{
	ThreadArg* pArg = (ThreadArg*)arg; // 스레드 인자를 ThreadArg 형식으로 변환합니다.
	CList<Frame, Frame&>* plist = pArg->pList; // 송신 Frame 패킷 리스트 주소를 가져옵니다.
	CUDPClientThdDlg* pDlg = (CUDPClientThdDlg*)pArg->pDlg; // 대화상자 주소를 가져옵니다.

	while (pArg->Thread_run) // 스레드 실행 플래그가 켜져 있는 동안 반복합니다.
	{
		Frame frame; // 송신 리스트에서 꺼낸 Frame 패킷을 저장합니다.
		BOOL hasFrame = FALSE; // 이번 반복에서 송신할 Frame이 있는지 표시합니다.

		tx_cs.Lock(); // 송신 리스트 접근을 잠급니다.
		if (!plist->IsEmpty()) // 송신할 Frame이 있으면 하나 꺼냅니다.
		{
			frame = plist->RemoveHead(); // 송신할 Frame을 리스트에서 제거하며 가져옵니다.
			hasFrame = TRUE; // 송신할 Frame이 있음을 표시합니다.
		}
		tx_cs.Unlock(); // 송신 리스트 잠금을 풉니다.

		if (hasFrame) // 송신할 Frame이 있을 때만 UDP 전송을 시도합니다.
		{
			if (pDlg->m_hSocket != INVALID_SOCKET) // UDP 소켓이 있을 때만 전송합니다.
			{
				BYTE nField0, nField1, nField2, nField3; // IP Address 컨트롤의 각 주소 값을 저장합니다.
				CString addr; // 서버 IP 주소 문자열입니다.
				pDlg->m_ipaddr.GetAddress(nField0, nField1, nField2, nField3); // IP Address 컨트롤에서 서버 IP를 가져옵니다.
				addr.Format(_T("%d.%d.%d.%d"), nField0, nField1, nField2, nField3); // 서버 IP를 문자열로 만듭니다.
				SOCKADDR_IN server_addr; // 메시지를 보낼 서버 주소 구조체입니다.
				memset(&server_addr, 0, sizeof(server_addr)); // 서버 주소 구조체를 초기화합니다.
				server_addr.sin_family = AF_INET; // IPv4 주소 체계를 사용합니다.
				server_addr.sin_port = htons(8000); // 서버 포트 번호를 8000번으로 설정합니다.
				InetPton(AF_INET, addr, &server_addr.sin_addr); // 서버 IP 주소를 저장합니다.
				if (pDlg->m_corruptNextPacket) // Checksum 시연 예약이 있으면 이번 송신 Frame 하나를 손상합니다.
				{
					pDlg->m_corruptNextPacket = FALSE; // 한 번만 손상되도록 예약 플래그를 즉시 끕니다.
					if (CorruptFrameForChecksumDemo(frame)) // Checksum은 유지하고 Payload만 일부 변경합니다.
						AppendCorruptPacketLog(pDlg->m_packet_log_edit, frame); // 손상된 Frame 정보를 패킷 로그창에 출력합니다.
					else // 손상할 Payload가 없으면 실패 로그를 남깁니다.
						AppendEditText(pDlg->m_packet_log_edit, _T("[CORRUPT SKIP] payload is empty\r\n"));
				}
				int sentBytes = sendto(pDlg->m_hSocket, (char*)&frame, sizeof(Frame), 0, (SOCKADDR*)&server_addr, sizeof(server_addr)); // Frame 구조체 전체를 UDP 패킷으로 보냅니다.
				if (sentBytes != SOCKET_ERROR) // 전송이 성공하면 패킷 로그를 남깁니다.
					AppendPacketLog(pDlg->m_packet_log_edit, _T("SEND"), frame, sentBytes);
				else // sendto 호출이 실패하면 전송 실패 로그를 남깁니다.
					AppendEditText(pDlg->m_packet_log_edit, _T("[SEND FAIL] sendto failed\r\n"));
			}
		}

		Sleep(10); // CPU 사용을 줄이기 위해 잠시 대기합니다.
	}

	return 0; // 스레드를 종료합니다.
}

UINT RXThread(LPVOID arg) // UDP로 받은 메시지를 화면에 출력하는 스레드 함수입니다.
{
	ThreadArg* pArg = (ThreadArg*)arg; // 스레드 인자를 ThreadArg 형식으로 변환합니다.
	CList<Frame, Frame&>* plist = pArg->pList; // 수신 Frame 패킷 리스트 주소를 가져옵니다.
	CUDPClientThdDlg* pDlg = (CUDPClientThdDlg*)pArg->pDlg; // 대화상자 주소를 가져옵니다.

	while (pArg->Thread_run) // 스레드 실행 플래그가 켜져 있는 동안 반복합니다.
	{
		pDlg->ProcessReceive(); // UDP 메시지를 수신하여 리스트에 저장합니다.

		while (TRUE) // 수신 리스트에 쌓인 Frame을 모두 출력합니다.
		{
			Frame frame; // 수신 리스트에서 꺼낸 Frame 패킷을 저장합니다.
			BOOL hasFrame = FALSE; // 이번 반복에서 출력할 Frame이 있는지 표시합니다.

			rx_cs.Lock(); // 수신 리스트 접근을 잠급니다.
			if (!plist->IsEmpty()) // 출력할 Frame이 있으면 하나 꺼냅니다.
			{
				frame = plist->RemoveHead(); // 출력할 Frame을 리스트에서 제거하며 가져옵니다.
				hasFrame = TRUE; // 출력할 Frame이 있음을 표시합니다.
			}
			rx_cs.Unlock(); // 수신 리스트 잠금을 풉니다.

			if (!hasFrame) // 더 이상 출력할 Frame이 없으면 반복을 끝냅니다.
				break;

			AppendPacketLog(pDlg->m_packet_log_edit, _T("RECV"), frame, sizeof(Frame)); // 받은 Frame 정보를 패킷 로그창에 출력합니다.
			CString completedText; // 재조립이 끝난 원본 메시지 문자열입니다.
			if (ProcessReassemblyFrame(pDlg->m_reassemblyList, pDlg->m_packet_log_edit, frame, completedText)) // 모든 조각이 모였을 때만 수신창에 원본 메시지를 출력합니다.
			{
				completedText += _T("\r\n"); // 채팅창에서 메시지 단위를 구분하기 위해 줄바꿈을 추가합니다.
				AppendEditText(pDlg->m_rx_edit, completedText); // 재조립된 원본 메시지를 수신창에 출력합니다.
			}
		}

		Sleep(10); // CPU 사용을 줄이기 위해 잠시 대기합니다.
	}

	return 0; // 스레드를 종료합니다.
}


// 응용 프로그램 정보에 사용되는 CAboutDlg 대화 상자입니다.

class CAboutDlg : public CDialogEx
{
public:
	CAboutDlg();

// 대화 상자 데이터입니다.
#ifdef AFX_DESIGN_TIME
	enum { IDD = IDD_ABOUTBOX };
#endif

	protected:
	virtual void DoDataExchange(CDataExchange* pDX);    // DDX/DDV 지원입니다.

// 구현입니다.
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


// CUDPClientThdDlg 대화 상자



CUDPClientThdDlg::CUDPClientThdDlg(CWnd* pParent /*=nullptr*/)
	: CDialogEx(IDD_UDPCLIENTTHD_DIALOG, pParent)
{
	m_hIcon = AfxGetApp()->LoadIcon(IDR_MAINFRAME);
	m_hSocket = INVALID_SOCKET; // UDP 소켓 핸들을 초기화합니다.
	m_nextMessageId = 1; // 첫 번째 송신 메시지 번호를 1로 초기화합니다.
	m_corruptNextPacket = FALSE; // 기본 상태에서는 송신 Frame을 손상하지 않도록 초기화합니다.
}

void CUDPClientThdDlg::DoDataExchange(CDataExchange* pDX)
{
	CDialogEx::DoDataExchange(pDX);
	DDX_Control(pDX, IDC_IPADDRESS, m_ipaddr); // 서버 IP 주소 컨트롤을 연결합니다.
	DDX_Control(pDX, IDC_EDIT1, m_tx_edit_short); // 보낼 메시지 입력 컨트롤을 연결합니다.
	DDX_Control(pDX, IDC_EDIT2, m_rx_edit); // 받은 메시지 출력 컨트롤을 연결합니다.
	DDX_Control(pDX, IDC_EDIT3, m_tx_edit); // 보낸 메시지 출력 컨트롤을 연결합니다.
	DDX_Control(pDX, IDC_PACKET_LOG, m_packet_log_edit); // 패킷 송수신 과정을 표시할 로그 컨트롤을 연결합니다.
}

BEGIN_MESSAGE_MAP(CUDPClientThdDlg, CDialogEx)
	ON_WM_SYSCOMMAND()
	ON_WM_PAINT()
	ON_WM_QUERYDRAGICON()
	ON_BN_CLICKED(IDC_SEND, &CUDPClientThdDlg::OnBnClickedSend)
	ON_BN_CLICKED(IDC_CLOSE, &CUDPClientThdDlg::OnBnClickedClose)
	ON_BN_CLICKED(IDC_CORRUPT_NEXT, &CUDPClientThdDlg::OnBnClickedCorruptNext)
	ON_EN_CHANGE(IDC_EDIT1, &CUDPClientThdDlg::OnEnChangeEdit1)
END_MESSAGE_MAP()


// CUDPClientThdDlg 메시지 처리기

BOOL CUDPClientThdDlg::OnInitDialog()
{
	CDialogEx::OnInitDialog();

	// 시스템 메뉴에 "정보..." 메뉴 항목을 추가합니다.

	// IDM_ABOUTBOX는 시스템 명령 범위에 있어야 합니다.
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

	// 이 대화 상자의 아이콘을 설정합니다.  응용 프로그램의 주 창이 대화 상자가 아닐 경우에는
	//  프레임워크가 이 작업을 자동으로 수행합니다.
	SetIcon(m_hIcon, TRUE);			// 큰 아이콘을 설정합니다.
	SetIcon(m_hIcon, FALSE);		// 작은 아이콘을 설정합니다.

	m_tx_edit_short.SetLimitText(MAX_MESSAGE_BYTES); // 영문 기준 256자 이상 입력되지 않도록 기본 제한을 설정합니다.

	CList<Frame, Frame&>* newlist = new CList<Frame, Frame&>; // 송신 Frame 패킷을 저장할 리스트를 생성합니다.
	arg1.pList = newlist; // 송신 스레드 인자에 송신 리스트를 저장합니다.
	arg1.Thread_run = 1; // 송신 스레드 실행 플래그를 켭니다.
	arg1.pDlg = this; // 송신 스레드 인자에 대화상자 주소를 저장합니다.

	CList<Frame, Frame&>* newlist2 = new CList<Frame, Frame&>; // 수신 Frame 패킷을 저장할 리스트를 생성합니다.
	arg2.pList = newlist2; // 수신 스레드 인자에 수신 리스트를 저장합니다.
	arg2.Thread_run = 1; // 수신 스레드 실행 플래그를 켭니다.
	arg2.pDlg = this; // 수신 스레드 인자에 대화상자 주소를 저장합니다.

	m_ipaddr.SetAddress(127, 0, 0, 1); // 실습 기본 서버 IP 주소를 127.0.0.1로 설정합니다.

	m_hSocket = socket(AF_INET, SOCK_DGRAM, 0); // UDP 소켓을 생성합니다.

	if (m_hSocket != INVALID_SOCKET) // 클라이언트는 임의 포트의 UDP 소켓을 사용합니다.
	{
		pThread1 = AfxBeginThread(TXThread, (LPVOID)&arg1); // 송신 스레드를 시작합니다.
		pThread2 = AfxBeginThread(RXThread, (LPVOID)&arg2); // 수신 스레드를 시작합니다.
		return TRUE; // 초기화 성공 후 대화상자를 실행합니다.
	}

	AfxMessageBox(_T("UDP 클라이언트 소켓 생성 실패")); // UDP 클라이언트 소켓 생성 실패를 알립니다.

	return TRUE;  // 포커스를 컨트롤에 설정하지 않으면 TRUE를 반환합니다.
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

// 대화 상자에 최소화 단추를 추가할 경우 아이콘을 그리려면
//  아래 코드가 필요합니다.  문서/뷰 모델을 사용하는 MFC 애플리케이션의 경우에는
//  프레임워크에서 이 작업을 자동으로 수행합니다.

void CUDPClientThdDlg::OnPaint()
{
	if (IsIconic())
	{
		CPaintDC dc(this); // 그리기를 위한 디바이스 컨텍스트입니다.

		SendMessage(WM_ICONERASEBKGND, reinterpret_cast<WPARAM>(dc.GetSafeHdc()), 0);

		// 클라이언트 사각형에서 아이콘을 가운데에 맞춥니다.
		int cxIcon = GetSystemMetrics(SM_CXICON);
		int cyIcon = GetSystemMetrics(SM_CYICON);
		CRect rect;
		GetClientRect(&rect);
		int x = (rect.Width() - cxIcon + 1) / 2;
		int y = (rect.Height() - cyIcon + 1) / 2;

		// 아이콘을 그립니다.
		dc.DrawIcon(x, y, m_hIcon);
	}
	else
	{
		CDialogEx::OnPaint();
	}
}

// 사용자가 최소화된 창을 끄는 동안에 커서가 표시되도록 시스템에서
//  이 함수를 호출합니다.
HCURSOR CUDPClientThdDlg::OnQueryDragIcon()
{
	return static_cast<HCURSOR>(m_hIcon);
}

void CUDPClientThdDlg::ProcessReceive() // UDP 메시지를 받아 수신 리스트에 넣습니다.
{
	Frame frame; // UDP에서 받은 패킷을 저장할 Frame 구조체입니다.
	int nbytes; // 실제로 받은 바이트 수입니다.

	if (m_hSocket == INVALID_SOCKET) // 소켓이 없으면 수신하지 않습니다.
		return;

	SOCKADDR_IN peer_addr; // 보낸 쪽 주소를 저장하는 구조체입니다.
	int peer_len = sizeof(peer_addr); // 보낸 쪽 주소 구조체 크기입니다.
	nbytes = recvfrom(m_hSocket, (char*)&frame, sizeof(Frame), 0, (SOCKADDR*)&peer_addr, &peer_len); // UDP 메시지를 Frame 구조체 크기만큼 받습니다.

	if (nbytes <= 0) // 받은 데이터가 없으면 함수를 끝냅니다.
		return;

	if (nbytes != sizeof(Frame)) // Frame 크기와 다르면 패킷으로 인정하지 않습니다.
	{
		AppendEditText(m_packet_log_edit, _T("[RECV DROP] invalid packet size\r\n")); // 잘못된 크기의 패킷을 로그에 남깁니다.
		return;
	}

	if (frame.payload_len <= 0 || frame.payload_len > FRAME_PAYLOAD_SIZE) // Payload 길이가 범위를 벗어나면 버립니다.
	{
		AppendEditText(m_packet_log_edit, _T("[RECV DROP] invalid payload length\r\n")); // 잘못된 Payload 길이를 로그에 남깁니다.
		return;
	}

	if (!IsValidSegmentHeader(frame)) // 메시지 번호와 조각 번호가 정상 범위인지 확인합니다.
	{
		AppendEditText(m_packet_log_edit, _T("[RECV DROP] invalid segment header\r\n")); // 잘못된 세그먼트 Header를 로그에 남깁니다.
		return;
	}

	int calculatedChecksum = 0; // 수신 Frame을 다시 계산한 Checksum 값을 저장합니다.
	if (!VerifyFrameChecksum(frame, calculatedChecksum)) // 수신 Checksum과 재계산 Checksum이 같은지 검증합니다.
	{
		AppendChecksumLog(m_packet_log_edit, _T("FAIL"), frame, calculatedChecksum); // Checksum 불일치 정보를 패킷 로그에 남깁니다.
		return;
	}

	AppendChecksumLog(m_packet_log_edit, _T("OK"), frame, calculatedChecksum); // Checksum 검증 성공 정보를 패킷 로그에 남깁니다.

	rx_cs.Lock(); // 수신 리스트 접근을 잠급니다.
	arg2.pList->AddTail(frame); // 받은 Frame 패킷을 수신 리스트에 추가합니다.
	rx_cs.Unlock(); // 수신 리스트 접근 잠금을 풉니다.
}

void CUDPClientThdDlg::OnBnClickedSend() // Send 버튼 클릭 시 메시지를 송신 리스트에 넣습니다.
{
	CString tx_message; // 사용자가 입력한 송신 메시지입니다.
	CList<Frame, Frame&> frameList; // 입력 메시지를 분할해 만든 Frame 패킷들을 임시로 저장합니다.
	int messageBytes = 0; // 전체 원본 메시지의 UTF-8 Byte 수를 저장합니다.
	int messageId = m_nextMessageId; // 이번 송신 메시지에 사용할 메시지 번호를 저장합니다.
	m_tx_edit_short.GetWindowTextW(tx_message); // 입력창의 문자열을 가져옵니다.

	if (!BuildFramesFromText(tx_message, frameList, messageId, messageBytes)) // 입력 문자열을 16Byte Frame들로 분할할 수 있는지 확인합니다.
	{
		AppendEditText(m_packet_log_edit, _T("[SEND SKIP] message must be 1-256 bytes\r\n")); // 전송하지 않은 이유를 패킷 로그에 남깁니다.
		m_tx_edit_short.SetFocus(); // 사용자가 바로 다시 입력할 수 있도록 포커스를 돌립니다.
		return;
	}

	m_nextMessageId++; // 다음 송신 메시지에 사용할 메시지 번호를 증가시킵니다.
	CString tx_log = tx_message + _T("\r\n"); // 송신 채팅창에 표시할 문자열에 줄바꿈을 추가합니다.
	AppendEditText(m_tx_edit, tx_log); // 사용자가 보낸 원문 메시지를 송신창에 출력합니다.
	AppendSegmentLog(m_packet_log_edit, messageId, messageBytes, (int)frameList.GetCount()); // 원본 메시지가 몇 개의 Frame으로 나뉘었는지 로그창에 출력합니다.

	POSITION pos = frameList.GetHeadPosition(); // 생성된 Frame들을 CREATE 로그로 남기기 위해 첫 위치를 가져옵니다.
	while (pos != NULL) // 생성된 모든 Frame 정보를 로그창에 출력합니다.
	{
		Frame frame = frameList.GetNext(pos); // 로그로 출력할 Frame을 임시 변수에 복사합니다.
		AppendPacketLog(m_packet_log_edit, _T("CREATE"), frame, sizeof(Frame)); // 생성된 Frame 정보를 패킷 로그창에 출력합니다.
	}

	tx_cs.Lock(); // 송신 리스트 접근을 잠급니다.
	pos = frameList.GetHeadPosition(); // 생성된 Frame들을 송신 리스트에 넣기 위해 첫 위치를 다시 가져옵니다.
	while (pos != NULL) // 생성된 모든 Frame을 송신 대기열에 추가합니다.
	{
		Frame frame = frameList.GetNext(pos); // 송신 리스트에 넣을 Frame을 임시 변수에 복사합니다.
		arg1.pList->AddTail(frame); // 송신할 Frame 패킷을 리스트에 추가합니다.
	}
	tx_cs.Unlock(); // 송신 리스트 접근 잠금을 풉니다.

	m_tx_edit_short.SetWindowTextW(_T("")); // 입력창을 비웁니다.
	m_tx_edit_short.SetFocus(); // 입력창으로 포커스를 이동합니다.
}

void CUDPClientThdDlg::OnBnClickedCorruptNext() // Corrupt 버튼 클릭 시 다음 송신 Frame 하나를 손상하도록 예약합니다.
{
	m_corruptNextPacket = TRUE; // TXThread가 다음 송신 Frame을 고의로 손상하도록 플래그를 켭니다.
	AppendEditText(m_packet_log_edit, _T("[CORRUPT READY] next outgoing packet will be damaged\r\n")); // 사용자가 시연 예약 상태를 볼 수 있도록 로그를 남깁니다.
}

void CUDPClientThdDlg::OnEnChangeEdit1() // 입력창의 UTF-8 Byte 수가 256Byte를 넘지 않도록 즉시 제한합니다.
{
	if (g_isTrimmingInput) // 코드가 입력창을 갱신하는 중이면 재진입을 막습니다.
		return;

	CString currentText; // 현재 입력창 문자열을 저장합니다.
	CString limitedText; // 256Byte 안에 들어가는 문자열만 저장합니다.
	m_tx_edit_short.GetWindowTextW(currentText); // 현재 입력창 문자열을 읽습니다.

	if (!LimitTextToMessageSize(currentText, limitedText)) // 이미 256Byte 이하이면 수정하지 않습니다.
		return;

	g_isTrimmingInput = TRUE; // SetWindowTextW로 발생할 EN_CHANGE 재진입을 막습니다.
	m_tx_edit_short.SetWindowTextW(limitedText); // 256Byte를 넘는 부분을 제거한 문자열로 입력창을 갱신합니다.
	m_tx_edit_short.SetSel(limitedText.GetLength(), limitedText.GetLength()); // 커서를 제한된 문자열 끝으로 이동합니다.
	g_isTrimmingInput = FALSE; // 입력 제한 작업이 끝났음을 표시합니다.
}

void CUDPClientThdDlg::OnBnClickedClose() // Close 버튼 클릭 시 소켓과 스레드를 종료합니다.
{
	arg1.Thread_run = 0; // 송신 스레드 종료 플래그를 설정합니다.
	arg2.Thread_run = 0; // 수신 스레드 종료 플래그를 설정합니다.

	if (m_hSocket != INVALID_SOCKET) // UDP 소켓이 있으면 정리합니다.
	{
		closesocket(m_hSocket); // UDP 소켓을 닫습니다.
		m_hSocket = INVALID_SOCKET; // 닫은 소켓 핸들을 초기화합니다.
	}

	CDialogEx::OnOK(); // 대화상자를 종료합니다.
}

