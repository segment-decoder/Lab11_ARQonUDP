
// UDPClientThdDlg.h: 헤더 파일
//

#pragma once

#include <afxtempl.h> // Frame 패킷 리스트를 저장하기 위해 MFC 템플릿 집합 클래스를 사용합니다.

const int FRAME_PAYLOAD_SIZE = 16; // 과제 조건에 맞춰 Frame 하나의 데이터 필드를 16Byte로 고정합니다.
const int MAX_MESSAGE_BYTES = 256; // 세그먼트 단계에서 한 번에 입력할 수 있는 전체 메시지 크기를 256Byte로 제한합니다.

struct Frame // UDP로 전송할 Header와 Payload를 하나로 묶은 패킷 구조체입니다.
{
	int seq_num; // 이후 ARQ 구현에서 사용할 순서 번호 Header 필드입니다.
	int ack_num; // 이후 ARQ 구현에서 사용할 응답 번호 Header 필드입니다.
	int checksum; // 이후 오류 검증 구현에서 사용할 Checksum Header 필드입니다.
	int msg_id; // 여러 Frame으로 나뉜 조각들이 같은 원본 메시지인지 구분하는 메시지 번호입니다.
	int frag_index; // 원본 메시지 안에서 현재 Frame이 몇 번째 조각인지 저장합니다.
	int frag_count; // 원본 메시지가 총 몇 개의 Frame으로 나뉘었는지 저장합니다.
	int payload_len; // Payload 버퍼 안에서 실제로 사용하는 Byte 수를 저장합니다.
	BYTE payload[FRAME_PAYLOAD_SIZE]; // 실제 채팅 데이터를 담는 16Byte 고정 데이터 필드입니다.

	Frame() // 새 Frame이 쓰레기 값을 갖지 않도록 초기화합니다.
	{
		seq_num = 0; // packet 단계에서는 순서 번호 기능을 아직 사용하지 않으므로 0으로 초기화합니다.
		ack_num = 0; // packet 단계에서는 ACK 번호 기능을 아직 사용하지 않으므로 0으로 초기화합니다.
		checksum = 0; // packet 단계에서는 Checksum 기능을 아직 사용하지 않으므로 0으로 초기화합니다.
		msg_id = 0; // 아직 어떤 원본 메시지에도 속하지 않은 상태로 초기화합니다.
		frag_index = 0; // 첫 번째 조각을 기본값으로 초기화합니다.
		frag_count = 0; // 아직 조각 총수가 정해지지 않은 상태로 초기화합니다.
		payload_len = 0; // 아직 담긴 Payload가 없음을 표시합니다.
		memset(payload, 0, sizeof(payload)); // Payload 버퍼를 0으로 초기화합니다.
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
};
