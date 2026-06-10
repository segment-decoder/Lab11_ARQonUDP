
// UDPClientThdDlg.h: 헤더 파일
//

#pragma once

#include <afxtempl.h> // Frame 패킷 리스트를 저장하기 위해 MFC 템플릿 집합 클래스를 사용합니다.

const int FRAME_PAYLOAD_SIZE = 16; // 과제 조건에 맞춰 Frame 하나의 데이터 필드를 16Byte로 고정합니다.
const int MAX_MESSAGE_BYTES = 256; // 세그먼트 단계에서 한 번에 입력할 수 있는 전체 메시지 크기를 256Byte로 제한합니다.
const int MAX_SEGMENT_COUNT = (MAX_MESSAGE_BYTES + FRAME_PAYLOAD_SIZE - 1) / FRAME_PAYLOAD_SIZE; // 256Byte 메시지가 최대 몇 개의 Frame으로 나뉘는지 계산합니다.
const DWORD REASSEMBLY_TIMEOUT_MS = 5000; // 재전송 구현 전 단계에서 너무 오래 남은 미완성 재조립 메시지를 정리하는 시간입니다.
const DWORD STOP_WAIT_TIMEOUT_MS = 1000; // Stop-and-Wait에서 ACK를 기다릴 최대 시간을 1초로 설정합니다.
const int STOP_WAIT_MAX_RETRY = 3; // Stop-and-Wait에서 같은 Frame을 최대 3번까지 재전송합니다.
const BYTE XOR_KEY = 0x5A; // Payload를 XOR 방식으로 암호화하고 복호화할 때 사용할 고정 1Byte Key입니다.

struct Frame // UDP로 전송할 Header와 Payload를 하나로 묶은 패킷 구조체입니다.
{
	int seq_num; // 송신 Frame마다 1씩 증가하는 순서 번호 Header 필드입니다.
	int ack_num; // 마지막으로 정상 수신한 상대 Frame 번호를 함께 싣는 ACK Header 필드입니다.
	int checksum; // Frame Header와 Payload 오류 검증에 사용할 16-bit Checksum Header 필드입니다.
	int msg_id; // 여러 Frame으로 나뉜 조각들이 같은 원본 메시지인지 구분하는 메시지 번호입니다.
	int frag_index; // 원본 메시지 안에서 현재 Frame이 몇 번째 조각인지 저장합니다.
	int frag_count; // 원본 메시지가 총 몇 개의 Frame으로 나뉘었는지 저장합니다.
	int payload_len; // Payload 버퍼 안에서 실제로 사용하는 Byte 수를 저장합니다.
	BYTE payload[FRAME_PAYLOAD_SIZE]; // 실제 채팅 데이터를 담는 16Byte 고정 데이터 필드입니다.

	Frame() // 새 Frame이 쓰레기 값을 갖지 않도록 초기화합니다.
	{
		seq_num = 0; // 아직 송신 순서 번호가 배정되지 않은 상태로 초기화합니다.
		ack_num = 0; // 아직 정상 수신한 상대 Frame이 없음을 표시합니다.
		checksum = 0; // Checksum 계산 전 기본값을 0으로 초기화합니다.
		msg_id = 0; // 아직 어떤 원본 메시지에도 속하지 않은 상태로 초기화합니다.
		frag_index = 0; // 첫 번째 조각을 기본값으로 초기화합니다.
		frag_count = 0; // 아직 조각 총수가 정해지지 않은 상태로 초기화합니다.
		payload_len = 0; // 아직 담긴 Payload가 없음을 표시합니다.
		memset(payload, 0, sizeof(payload)); // Payload 버퍼를 0으로 초기화합니다.
	}
};

struct ReassemblyMessage // 수신한 여러 Frame 조각을 원본 메시지 단위로 모아두는 구조체입니다.
{
	int msg_id; // 재조립 중인 원본 메시지 번호입니다.
	int frag_count; // 원본 메시지를 완성하는 데 필요한 전체 조각 수입니다.
	int received_count; // 현재까지 받은 조각 수입니다.
	int total_payload_len; // 재조립 후 UTF-8 Payload 전체 Byte 수입니다.
	BOOL received[MAX_SEGMENT_COUNT]; // 각 조각 번호를 이미 받았는지 표시합니다.
	Frame fragments[MAX_SEGMENT_COUNT]; // 수신한 Frame 조각을 frag_index 위치에 저장합니다.
	DWORD last_update_tick; // 마지막으로 조각을 받은 시간을 저장해 오래된 미완성 메시지를 정리합니다.

	ReassemblyMessage() // 새 재조립 버퍼가 쓰레기 값을 갖지 않도록 초기화합니다.
	{
		msg_id = 0; // 아직 어떤 원본 메시지도 담당하지 않는 상태로 초기화합니다.
		frag_count = 0; // 필요한 전체 조각 수가 아직 정해지지 않은 상태로 초기화합니다.
		received_count = 0; // 아직 받은 조각이 없음을 표시합니다.
		total_payload_len = 0; // 아직 누적된 Payload Byte가 없음을 표시합니다.
		last_update_tick = 0; // 아직 수신 시간이 기록되지 않았음을 표시합니다.

		for (int index = 0; index < MAX_SEGMENT_COUNT; index++) // 모든 조각 저장 위치를 초기화합니다.
		{
			received[index] = FALSE; // 해당 조각을 아직 받지 않았다고 표시합니다.
			fragments[index] = Frame(); // 해당 위치의 Frame 데이터를 빈 Frame으로 초기화합니다.
		}
	}
};

struct ThreadArg
{
	CList<Frame, Frame&>* pList; // 송신 또는 수신 Frame 패킷을 보관하는 리스트입니다.
	CDialogEx* pDlg; // 스레드에서 대화상자 객체에 접근하기 위한 포인터입니다.
	int Thread_run; // 스레드 실행 여부를 저장합니다.
};

// CUDPClientThdDlg 대화 상자
class CUDPClientThdDlg : public CDialogEx
{
// 생성입니다.
public:
	CUDPClientThdDlg(CWnd* pParent = nullptr);	// 표준 생성자입니다.

// 대화 상자 데이터입니다.
#ifdef AFX_DESIGN_TIME
	enum { IDD = IDD_UDPCLIENTTHD_DIALOG };
#endif

	protected:
	virtual void DoDataExchange(CDataExchange* pDX);	// DDX/DDV 지원입니다.


// 구현입니다.
protected:
	HICON m_hIcon;

	// 생성된 메시지 맵 함수
	virtual BOOL OnInitDialog();
	afx_msg void OnSysCommand(UINT nID, LPARAM lParam);
	afx_msg void OnPaint();
	afx_msg HCURSOR OnQueryDragIcon();
	afx_msg void OnBnClickedSend(); // Send 버튼 클릭 시 송신 리스트에 메시지를 넣습니다.
	afx_msg void OnBnClickedClose(); // Close 버튼 클릭 시 소켓과 스레드를 종료합니다.
	afx_msg void OnBnClickedCorruptNext(); // Corrupt 버튼 클릭 시 다음 송신 Frame을 일부러 손상하도록 예약합니다.
	afx_msg void OnEnChangeEdit1(); // 입력창 내용이 바뀔 때 256Byte 초과 입력을 제한합니다.
	DECLARE_MESSAGE_MAP()

public:
	void ProcessReceive(); // UDP 패킷을 받아 수신 리스트에 넣습니다.
	CWinThread* pThread1, *pThread2; // 송신 스레드와 수신 스레드 객체 주소입니다.
	ThreadArg arg1, arg2; // 송신 스레드와 수신 스레드에 전달할 인자입니다.
	SOCKET m_hSocket; // UDP 송수신에 사용하는 소켓 핸들입니다.
	CIPAddressCtrl m_ipaddr; // 서버 IP 주소를 입력하는 IP Address 컨트롤입니다.
	CEdit m_rx_edit; // 받은 메시지를 출력하는 편집 컨트롤입니다.
	CEdit m_tx_edit; // 보낸 메시지를 출력하는 편집 컨트롤입니다.
	CEdit m_tx_edit_short; // 보낼 메시지를 입력하는 편집 컨트롤입니다.
	CEdit m_packet_log_edit; // 패킷 생성, 송신, 수신 과정을 출력하는 로그 전용 편집 컨트롤입니다.
	int m_nextMessageId; // 다음에 송신할 원본 메시지에 붙일 메시지 번호입니다.
	int m_nextSeqNum; // 다음 송신 Frame에 붙일 순서 번호입니다.
	int m_expectedSeqNum; // 다음에 정상 수신할 것으로 기대하는 상대 Frame 순서 번호입니다.
	int m_lastAckNum; // 마지막으로 정상 수신해 ACK로 알려줄 상대 Frame 순서 번호입니다.
	int m_lastReceivedAckNum; // 상대가 Piggyback으로 알려준 마지막 ACK 번호입니다.
	BOOL m_waitingAck; // Stop-and-Wait에서 현재 ACK를 기다리는 중인지 저장합니다.
	Frame m_waitFrame; // Stop-and-Wait에서 ACK를 받을 때까지 보관할 마지막 송신 Frame입니다.
	int m_waitAckNum; // Stop-and-Wait에서 기다리는 ACK 번호를 저장합니다.
	int m_retryCount; // Stop-and-Wait에서 현재 Frame을 몇 번 재전송했는지 저장합니다.
	DWORD m_lastSendTick; // Stop-and-Wait에서 마지막 송신 시각을 저장해 Timeout을 판단합니다.
	BOOL m_corruptNextPacket; // Checksum 시연을 위해 다음 송신 Frame 하나를 일부러 손상할지 저장합니다.
	CList<ReassemblyMessage, ReassemblyMessage&> m_reassemblyList; // 수신한 Frame 조각을 원본 메시지별로 임시 보관합니다.
};
