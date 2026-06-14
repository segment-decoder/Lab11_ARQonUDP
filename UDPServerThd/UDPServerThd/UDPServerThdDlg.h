// UDPServerThdDlg.h: 대화상자, Frame 구조체, ARQ 상태 변수를 선언한 헤더 파일입니다.
#pragma once // 헤더가 한 번만 포함되도록 합니다.

#include <afxtempl.h> // MFC 리스트 컨테이너를 사용하기 위해 포함합니다.

const int FRAME_PAYLOAD_SIZE = 16; // 과제 조건에 맞춰 Frame payload를 16Byte로 고정합니다.
const int MAX_MESSAGE_BYTES = 256; // 한 번에 보낼 수 있는 원본 메시지 크기를 256Byte로 제한합니다.
const int MAX_SEGMENT_COUNT = (MAX_MESSAGE_BYTES + FRAME_PAYLOAD_SIZE - 1) / FRAME_PAYLOAD_SIZE; // 256Byte 메시지를 16Byte씩 나눌 때 필요한 최대 fragment 수입니다.
const DWORD REASSEMBLY_TIMEOUT_MS = 5000; // 오래 남은 미완성 reassembly 버퍼를 정리할 기준 시간입니다.
const DWORD STOP_WAIT_TIMEOUT_MS = 1000; // Stop-and-Wait에서 ACK를 기다릴 timeout 시간입니다.
const int STOP_WAIT_MAX_RETRY = 3; // 같은 Frame을 재전송할 최대 횟수입니다.
const BYTE XOR_KEY = 0x5A; // payload XOR 변환에 사용할 1Byte key입니다.
const int TIMER_INTERVAL_MS = 10; // TimerThread가 ACK 대기 시간을 누적하는 간격입니다.

struct Frame // ARQ header와 16Byte payload를 묶은 응용계층 Frame입니다.
{
	int seq_num; // Frame의 송신 순서 번호입니다.
	int ack_num; // 상대에게 알려줄 마지막 정상 수신 Frame 번호입니다.
	int checksum; // header와 payload 오류 검출에 사용할 checksum 값입니다.
	int msg_id; // 분할된 fragment들이 같은 원본 메시지인지 구분하는 번호입니다.
	int frag_index; // 원본 메시지 안에서 현재 fragment의 위치입니다.
	int frag_count; // 원본 메시지를 구성하는 전체 fragment 개수입니다.
	int payload_len; // payload 배열에서 실제로 사용하는 Byte 수입니다.
	BYTE payload[FRAME_PAYLOAD_SIZE]; // 실제 채팅 데이터를 담는 16Byte payload 버퍼입니다.

	Frame() // Frame 생성 시 header와 payload를 기본값으로 초기화합니다.
	{
		seq_num = 0;
		ack_num = 0;
		checksum = 0;
		msg_id = 0;
		frag_index = 0;
		frag_count = 0;
		payload_len = 0;
		memset(payload, 0, sizeof(payload)); // 버퍼를 0으로 초기화합니다.
	}
};

struct ReassemblyMessage // 같은 msg_id의 fragment들을 모아두는 reassembly 버퍼입니다.
{
	int msg_id; // 분할된 fragment들이 같은 원본 메시지인지 구분하는 번호입니다.
	int frag_count; // 원본 메시지를 구성하는 전체 fragment 개수입니다.
	int received_count; // 현재까지 도착한 fragment 개수입니다.
	int total_payload_len; // reassembly 후 전체 payload Byte 수입니다.
	BOOL received[MAX_SEGMENT_COUNT]; // 각 fragment가 도착했는지 표시하는 배열입니다.
	Frame fragments[MAX_SEGMENT_COUNT]; // 수신한 fragment를 frag_index 위치에 저장하는 배열입니다.
	DWORD last_update_tick; // 미완성 reassembly 버퍼의 마지막 갱신 시각입니다.

	ReassemblyMessage() // reassembly 버퍼를 빈 상태로 초기화합니다.
	{
		msg_id = 0;
		frag_count = 0;
		received_count = 0;
		total_payload_len = 0;
		last_update_tick = 0;

		for (int index = 0; index < MAX_SEGMENT_COUNT; index++) // 256Byte 메시지가 최대 몇 개의 fragment로 나뉘는지 계산합니다.
		{
			received[index] = FALSE;
			fragments[index] = Frame();
		}
	}
};

struct ThreadArg // 송신, 수신, 타이머 스레드에 넘길 인자 구조체입니다.
{
	CList<Frame, Frame&>* pList; // 송신 또는 수신 Frame 큐를 가리키는 포인터입니다.
	CDialogEx* pDlg; // 스레드에서 접근할 대화상자 포인터입니다.
	int Thread_run; // 스레드 실행 여부를 나타내는 플래그입니다.
};


class CUDPServerThdDlg : public CDialogEx
{

public:
	CUDPServerThdDlg(CWnd* pParent = nullptr);


#ifdef AFX_DESIGN_TIME
	enum { IDD = IDD_UDPSERVERTHD_DIALOG };
#endif

	protected:
	virtual void DoDataExchange(CDataExchange* pDX);



protected:
	HICON m_hIcon;


	virtual BOOL OnInitDialog();
	afx_msg void OnSysCommand(UINT nID, LPARAM lParam);
	afx_msg void OnPaint();
	afx_msg HCURSOR OnQueryDragIcon();
	afx_msg void OnBnClickedSend(); // Send 버튼 클릭 시 입력 메시지를 Frame으로 나누어 송신 큐에 넣습니다.
	afx_msg void OnBnClickedClose(); // 스레드를 종료하고 UDP 소켓을 닫습니다.
	afx_msg void OnBnClickedCorruptNext(); // Corrupt 버튼 클릭 시 다음 Frame에 적용할 오류 주입을 예약합니다.
	afx_msg void OnBnClickedVerboseLog(); // 상세 로그와 요약 로그 모드를 전환합니다.
	afx_msg void OnEnChangeEdit1(); // 입력창 내용이 256Byte를 넘지 않도록 제한합니다.
	DECLARE_MESSAGE_MAP()

public:
	void ProcessReceive(); // UDP Frame을 수신한 뒤 크기, checksum, seq/ACK, reassembly 순서로 검증합니다.
	CWinThread* pThread1, *pThread2; // 송신 스레드와 수신 스레드 객체 포인터입니다.
	ThreadArg arg1, arg2; // 송신 스레드와 수신 스레드에 넘길 인자입니다.
	SOCKET m_hSocket; // UDP 송수신에 사용할 소켓 핸들입니다.
	CEdit m_rx_edit; // 수신 메시지를 표시하는 출력창입니다.
	CEdit m_tx_edit; // 송신한 메시지를 표시하는 출력창입니다.
	CEdit m_tx_edit_short; // 보낼 메시지를 입력하는 입력창입니다.
	CEdit m_packet_log_edit; // Frame 생성, 송신, 수신 과정을 보여주는 로그창입니다.
	CString m_clientAddr; // 화면 출력에 사용할 문자열 변수입니다.
	UINT m_clientPort;
	int m_nextMessageId; // 다음 원본 메시지에 붙일 msg_id입니다.
	int m_nextSeqNum; // 다음 송신 Frame에 붙일 seq 번호입니다.
	int m_expectedSeqNum; // 다음에 정상 수신할 것으로 기대하는 seq 번호입니다.
	int m_lastAckNum; // 마지막으로 정상 수신한 상대 Frame 번호입니다.
	int m_lastReceivedAckNum; // 상대가 보낸 ACK 중 마지막으로 처리한 번호입니다.
	BOOL m_waitingAck; // Stop-and-Wait에서 ACK 대기 중인지 나타냅니다.
	Frame m_waitFrame; // timeout 발생 시 재전송할 Frame을 보관합니다.
	int m_waitAckNum; // 현재 기다리는 ACK 번호입니다.
	int m_retryCount; // 현재 Frame의 재전송 횟수입니다.
	DWORD m_lastSendTick; // 마지막 송신 시각을 저장하는 변수입니다.
	int m_injectErrorType; // combo box에서 선택한 오류 주입 종류입니다.
	BOOL m_injectArmed; // 다음 Frame에 오류를 주입할지 나타내는 플래그입니다.
	CComboBox m_errorTypeCombo; // 오류 종류를 선택하는 combo box입니다.
	CButton m_verboseLog; // 요약/상세 로그 모드를 전환하는 checkbox입니다.
	CWinThread* pThread3; // timeout 측정을 담당하는 TimerThread 객체 포인터입니다.
	ThreadArg arg3; // TimerThread에 넘길 인자입니다.
	int m_elapsedTimeoutMs; // ACK 대기 누적 시간을 ms 단위로 저장합니다.
	BOOL m_timeoutFired; // TimerThread가 timeout을 감지했음을 나타내는 플래그입니다.
	CList<ReassemblyMessage, ReassemblyMessage&> m_reassemblyList; // reassembly 상태를 담을 변수입니다.
};
