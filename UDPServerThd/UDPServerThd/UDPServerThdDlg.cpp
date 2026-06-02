
// UDPServerThdDlg.cpp: 구현 파일
//

#include "pch.h"
#include "framework.h"
#include "UDPServerThd.h"
#include "UDPServerThdDlg.h"
#include "afxdialogex.h"

#pragma comment(lib, "ws2_32.lib")

#ifdef _DEBUG
#define new DEBUG_NEW
#endif

CCriticalSection tx_cs; // 송신 리스트 접근을 보호하는 임계 영역입니다.
CCriticalSection rx_cs; // 수신 리스트 접근을 보호하는 임계 영역입니다.

UINT TXThread(LPVOID arg) // 송신 리스트의 메시지를 UDP로 전송하는 스레드 함수입니다.
{
	ThreadArg* pArg = (ThreadArg*)arg; // 스레드 인자를 ThreadArg 형식으로 변환합니다.
	CStringList* plist = pArg->pList; // 송신 메시지 리스트 주소를 가져옵니다.
	CUDPServerThdDlg* pDlg = (CUDPServerThdDlg*)pArg->pDlg; // 대화상자 주소를 가져옵니다.

	while (pArg->Thread_run) // 스레드 실행 플래그가 켜져 있는 동안 반복합니다.
	{
		POSITION pos = plist->GetHeadPosition(); // 송신 리스트의 첫 위치를 가져옵니다.
		POSITION current_pos; // 삭제할 현재 위치를 저장합니다.

		while (pos != NULL) // 송신 리스트에 메시지가 있으면 처리합니다.
		{
			current_pos = pos; // 현재 리스트 위치를 저장합니다.
			tx_cs.Lock(); // 송신 리스트 접근을 잠급니다.
			CString str = plist->GetNext(pos); // 송신할 문자열을 꺼냅니다.
			tx_cs.Unlock(); // 송신 리스트 접근 잠금을 풉니다.

			if (pDlg->m_hSocket != INVALID_SOCKET && !pDlg->m_clientAddr.IsEmpty()) // 소켓과 클라이언트 주소가 있을 때만 전송합니다.
			{
				SOCKADDR_IN client_addr; // 메시지를 보낼 클라이언트 주소 구조체입니다.
				memset(&client_addr, 0, sizeof(client_addr)); // 주소 구조체를 초기화합니다.
				client_addr.sin_family = AF_INET; // IPv4 주소 체계를 사용합니다.
				client_addr.sin_port = htons(pDlg->m_clientPort); // 클라이언트 포트 번호를 네트워크 바이트 순서로 저장합니다.
				InetPton(AF_INET, pDlg->m_clientAddr, &client_addr.sin_addr); // 클라이언트 IP 주소를 저장합니다.
				sendto(pDlg->m_hSocket, (char*)(LPCTSTR)str, (str.GetLength() + 1) * sizeof(TCHAR), 0, (SOCKADDR*)&client_addr, sizeof(client_addr)); // 마지막 클라이언트로 UDP 메시지를 보냅니다.
				int len = pDlg->m_tx_edit.GetWindowTextLengthW(); // 송신 출력창의 끝 위치를 구합니다.
				pDlg->m_tx_edit.SetSel(len, len); // 송신 출력창 커서를 끝으로 이동합니다.
				pDlg->m_tx_edit.ReplaceSel(str); // 보낸 메시지를 송신 출력창에 출력합니다.
			}

			tx_cs.Lock(); // 송신 리스트 삭제를 위해 잠급니다.
			plist->RemoveAt(current_pos); // 전송한 메시지를 리스트에서 삭제합니다.
			tx_cs.Unlock(); // 송신 리스트 잠금을 풉니다.
		}

		Sleep(10); // CPU 사용을 줄이기 위해 잠시 대기합니다.
	}

	return 0; // 스레드를 종료합니다.
}

UINT RXThread(LPVOID arg) // UDP로 받은 메시지를 화면에 출력하는 스레드 함수입니다.
{
	ThreadArg* pArg = (ThreadArg*)arg; // 스레드 인자를 ThreadArg 형식으로 변환합니다.
	CStringList* plist = pArg->pList; // 수신 메시지 리스트 주소를 가져옵니다.
	CUDPServerThdDlg* pDlg = (CUDPServerThdDlg*)pArg->pDlg; // 대화상자 주소를 가져옵니다.

	while (pArg->Thread_run) // 스레드 실행 플래그가 켜져 있는 동안 반복합니다.
	{
		pDlg->ProcessReceive(); // UDP 메시지를 수신하여 리스트에 저장합니다.
		POSITION pos = plist->GetHeadPosition(); // 수신 리스트의 첫 위치를 가져옵니다.
		POSITION current_pos; // 삭제할 현재 위치를 저장합니다.

		while (pos != NULL) // 수신 리스트에 메시지가 있으면 처리합니다.
		{
			current_pos = pos; // 현재 리스트 위치를 저장합니다.
			rx_cs.Lock(); // 수신 리스트 접근을 잠급니다.
			CString str = plist->GetNext(pos); // 출력할 문자열을 꺼냅니다.
			rx_cs.Unlock(); // 수신 리스트 접근 잠금을 풉니다.

			int len = pDlg->m_rx_edit.GetWindowTextLengthW(); // 수신 출력창의 끝 위치를 구합니다.
			pDlg->m_rx_edit.SetSel(len, len); // 수신 출력창 커서를 끝으로 이동합니다.
			pDlg->m_rx_edit.ReplaceSel(str); // 받은 메시지를 수신 출력창에 출력합니다.

			rx_cs.Lock(); // 수신 리스트 삭제를 위해 잠급니다.
			plist->RemoveAt(current_pos); // 출력한 메시지를 리스트에서 삭제합니다.
			rx_cs.Unlock(); // 수신 리스트 잠금을 풉니다.
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


// CUDPServerThdDlg 대화 상자



CUDPServerThdDlg::CUDPServerThdDlg(CWnd* pParent /*=nullptr*/)
	: CDialogEx(IDD_UDPSERVERTHD_DIALOG, pParent)
{
	m_hIcon = AfxGetApp()->LoadIcon(IDR_MAINFRAME);
	m_hSocket = INVALID_SOCKET; // UDP 소켓 핸들을 초기화합니다.
	m_clientPort = 0; // 마지막 클라이언트 포트 번호를 초기화합니다.
}

void CUDPServerThdDlg::DoDataExchange(CDataExchange* pDX)
{
	CDialogEx::DoDataExchange(pDX);
	DDX_Control(pDX, IDC_EDIT1, m_tx_edit_short); // 보낼 메시지 입력 컨트롤을 연결합니다.
	DDX_Control(pDX, IDC_EDIT2, m_rx_edit); // 받은 메시지 출력 컨트롤을 연결합니다.
	DDX_Control(pDX, IDC_EDIT3, m_tx_edit); // 보낸 메시지 출력 컨트롤을 연결합니다.
}

BEGIN_MESSAGE_MAP(CUDPServerThdDlg, CDialogEx)
	ON_WM_SYSCOMMAND()
	ON_WM_PAINT()
	ON_WM_QUERYDRAGICON()
	ON_BN_CLICKED(IDC_SEND, &CUDPServerThdDlg::OnBnClickedSend)
	ON_BN_CLICKED(IDC_CLOSE, &CUDPServerThdDlg::OnBnClickedClose)
END_MESSAGE_MAP()


// CUDPServerThdDlg 메시지 처리기

BOOL CUDPServerThdDlg::OnInitDialog()
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

	CStringList* newlist = new CStringList; // 송신 메시지를 저장할 리스트를 생성합니다.
	arg1.pList = newlist; // 송신 스레드 인자에 송신 리스트를 저장합니다.
	arg1.Thread_run = 1; // 송신 스레드 실행 플래그를 켭니다.
	arg1.pDlg = this; // 송신 스레드 인자에 대화상자 주소를 저장합니다.

	CStringList* newlist2 = new CStringList; // 수신 메시지를 저장할 리스트를 생성합니다.
	arg2.pList = newlist2; // 수신 스레드 인자에 수신 리스트를 저장합니다.
	arg2.Thread_run = 1; // 수신 스레드 실행 플래그를 켭니다.
	arg2.pDlg = this; // 수신 스레드 인자에 대화상자 주소를 저장합니다.

	m_hSocket = socket(AF_INET, SOCK_DGRAM, 0); // UDP 소켓을 생성합니다.
	SOCKADDR_IN server_addr; // 서버 주소 구조체입니다.
	memset(&server_addr, 0, sizeof(server_addr)); // 서버 주소 구조체를 초기화합니다.
	server_addr.sin_family = AF_INET; // IPv4 주소 체계를 사용합니다.
	server_addr.sin_addr.s_addr = htonl(INADDR_ANY); // 모든 IP 주소에서 들어오는 패킷을 받습니다.
	server_addr.sin_port = htons(8000); // 서버 포트 번호를 8000번으로 설정합니다.

	if (m_hSocket != INVALID_SOCKET && bind(m_hSocket, (SOCKADDR*)&server_addr, sizeof(server_addr)) != SOCKET_ERROR) // 서버는 8000번 포트에 UDP 소켓을 연결합니다.
	{
		pThread1 = AfxBeginThread(TXThread, (LPVOID)&arg1); // 송신 스레드를 시작합니다.
		pThread2 = AfxBeginThread(RXThread, (LPVOID)&arg2); // 수신 스레드를 시작합니다.
		return TRUE; // 초기화 성공 후 대화상자를 실행합니다.
	}

	AfxMessageBox(_T("UDP 서버 소켓 생성 실패")); // UDP 서버 소켓 생성 실패를 알립니다.

	return TRUE;  // 포커스를 컨트롤에 설정하지 않으면 TRUE를 반환합니다.
}

void CUDPServerThdDlg::OnSysCommand(UINT nID, LPARAM lParam)
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

void CUDPServerThdDlg::OnPaint()
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
HCURSOR CUDPServerThdDlg::OnQueryDragIcon()
{
	return static_cast<HCURSOR>(m_hIcon);
}

void CUDPServerThdDlg::ProcessReceive() // UDP 메시지를 받고 보낸 클라이언트 주소를 저장합니다.
{
	TCHAR pBuf[1024 + 1]; // 받은 UDP 데이터를 저장할 버퍼입니다.
	CString strData; // 화면에 출력할 수신 문자열입니다.
	CString PeerAddr; // 메시지를 보낸 클라이언트 IP 주소입니다.
	UINT PeerPort; // 메시지를 보낸 클라이언트 포트 번호입니다.
	int nbytes; // 실제로 받은 바이트 수입니다.

	if (m_hSocket == INVALID_SOCKET) // 소켓이 없으면 수신하지 않습니다.
		return;

	SOCKADDR_IN peer_addr; // 보낸 쪽 주소를 저장하는 구조체입니다.
	int peer_len = sizeof(peer_addr); // 보낸 쪽 주소 구조체 크기입니다.
	nbytes = recvfrom(m_hSocket, (char*)pBuf, 1024 * sizeof(TCHAR), 0, (SOCKADDR*)&peer_addr, &peer_len); // UDP 메시지와 보낸 쪽 주소를 받습니다.

	if (nbytes <= 0) // 받은 데이터가 없으면 함수를 끝냅니다.
		return;

	pBuf[nbytes / sizeof(TCHAR)] = NULL; // 문자열 끝을 표시합니다.
	strData = (LPCTSTR)pBuf; // 받은 버퍼를 CString으로 변환합니다.
	TCHAR addrText[32]; // 보낸 클라이언트 IP 주소 문자열 버퍼입니다.
	InetNtop(AF_INET, &peer_addr.sin_addr, addrText, 32); // 보낸 클라이언트 IP 주소를 문자열로 변환합니다.
	PeerAddr = addrText; // 변환된 클라이언트 IP 주소를 저장합니다.
	PeerPort = ntohs(peer_addr.sin_port); // 보낸 클라이언트 포트 번호를 호스트 바이트 순서로 변환합니다.
	m_clientAddr = PeerAddr; // 마지막으로 메시지를 보낸 클라이언트 IP를 저장합니다.
	m_clientPort = PeerPort; // 마지막으로 메시지를 보낸 클라이언트 포트를 저장합니다.

	rx_cs.Lock(); // 수신 리스트 접근을 잠급니다.
	arg2.pList->AddTail((LPCTSTR)strData); // 받은 메시지를 수신 리스트에 추가합니다.
	rx_cs.Unlock(); // 수신 리스트 접근 잠금을 풉니다.
}

void CUDPServerThdDlg::OnBnClickedSend() // Send 버튼 클릭 시 메시지를 송신 리스트에 넣습니다.
{
	CString tx_message; // 사용자가 입력한 송신 메시지입니다.
	m_tx_edit_short.GetWindowTextW(tx_message); // 입력창의 문자열을 가져옵니다.
	tx_message += _T("\r\n"); // 출력과 전송을 위해 줄바꿈을 추가합니다.

	tx_cs.Lock(); // 송신 리스트 접근을 잠급니다.
	arg1.pList->AddTail(tx_message); // 송신할 메시지를 리스트에 추가합니다.
	tx_cs.Unlock(); // 송신 리스트 접근 잠금을 풉니다.

	m_tx_edit_short.SetWindowTextW(_T("")); // 입력창을 비웁니다.
	m_tx_edit_short.SetFocus(); // 입력창으로 포커스를 이동합니다.
}

void CUDPServerThdDlg::OnBnClickedClose() // Close 버튼 클릭 시 소켓과 스레드를 종료합니다.
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

