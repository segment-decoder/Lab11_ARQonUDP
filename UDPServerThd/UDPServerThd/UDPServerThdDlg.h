
// UDPServerThdDlg.h: 헤더 파일
//

#pragma once

struct ThreadArg
{
	CStringList* pList; // 송신 또는 수신 메시지를 보관하는 리스트입니다.
	CDialogEx* pDlg; // 스레드에서 대화상자 객체에 접근하기 위한 포인터입니다.
	int Thread_run; // 스레드 실행 여부를 저장합니다.
};

// CUDPServerThdDlg 대화 상자
class CUDPServerThdDlg : public CDialogEx
{
// 생성입니다.
public:
	CUDPServerThdDlg(CWnd* pParent = nullptr);	// 표준 생성자입니다.

// 대화 상자 데이터입니다.
#ifdef AFX_DESIGN_TIME
	enum { IDD = IDD_UDPSERVERTHD_DIALOG };
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
	DECLARE_MESSAGE_MAP()

public:
	void ProcessReceive(); // UDP 패킷을 받아 수신 리스트에 넣습니다.
	CWinThread* pThread1, *pThread2; // 송신 스레드와 수신 스레드 객체 주소입니다.
	ThreadArg arg1, arg2; // 송신 스레드와 수신 스레드에 전달할 인자입니다.
	SOCKET m_hSocket; // UDP 송수신에 사용하는 소켓 핸들입니다.
	CEdit m_rx_edit; // 받은 메시지를 출력하는 편집 컨트롤입니다.
	CEdit m_tx_edit; // 보낸 메시지를 출력하는 편집 컨트롤입니다.
	CEdit m_tx_edit_short; // 보낼 메시지를 입력하는 편집 컨트롤입니다.
	CString m_clientAddr; // 마지막으로 메시지를 보낸 클라이언트 IP 주소입니다.
	UINT m_clientPort; // 마지막으로 메시지를 보낸 클라이언트 포트 번호입니다.
};
