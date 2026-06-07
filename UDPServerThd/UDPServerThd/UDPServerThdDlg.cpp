
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

BOOL LimitTextToPayloadSize(const CString& source, CString& limitedText) // 입력 문자열을 Frame Payload 16Byte 안에 들어가도록 자릅니다.
{
	CStringW sourceText(source); // Unicode CString을 UTF-8 Byte 계산용 문자열로 변환합니다.
	CStringW limitedWideText; // 16Byte 안에 들어가는 문자만 누적합니다.
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
		if (usedBytes + unitBytes > FRAME_PAYLOAD_SIZE) // 16Byte를 넘으면 더 이상 입력을 받지 않습니다.
			break;

		limitedWideText += unitText; // 제한 안에 들어가는 문자를 결과 문자열에 추가합니다.
		usedBytes += unitBytes; // 누적 Byte 수를 갱신합니다.
		index += unitCount; // 다음 문자 단위로 이동합니다.
	}

	limitedText = CString(limitedWideText); // 제한된 Unicode 문자열을 MFC CString으로 되돌립니다.
	return sourceText.GetLength() != limitedWideText.GetLength(); // 실제로 잘라낸 문자가 있는지 반환합니다.
}

BOOL BuildFrameFromText(const CString& text, Frame& frame) // 입력 문자열을 16Byte Payload를 가진 Frame 패킷으로 변환합니다.
{
	CStringW wideText(text); // UI 입력 문자열을 UTF-8 변환용 Unicode 문자열로 준비합니다.
	int utf8Bytes = GetUtf8ByteCount(wideText); // 전송 Payload에 들어갈 실제 Byte 수를 계산합니다.

	if (utf8Bytes <= 0) // 빈 메시지는 Frame으로 만들지 않습니다.
		return FALSE;

	if (utf8Bytes > FRAME_PAYLOAD_SIZE) // UI 제한을 우회한 16Byte 초과 입력은 송신하지 않습니다.
		return FALSE;

	frame = Frame(); // 기존 Frame 내용을 초기화합니다.
	frame.payload_len = utf8Bytes; // 수신자가 실제 Payload 길이를 알 수 있도록 Header 값을 채웁니다.
	WideCharToMultiByte(CP_UTF8, 0, wideText, wideText.GetLength(), (LPSTR)frame.payload, FRAME_PAYLOAD_SIZE, NULL, NULL); // 입력 문자열을 UTF-8 Byte로 Payload에 저장합니다.
	return TRUE; // Frame 생성 성공을 알립니다.
}

CString FramePayloadToText(const Frame& frame) // 수신한 Frame Payload를 화면 출력용 CString으로 복원합니다.
{
	if (frame.payload_len <= 0 || frame.payload_len > FRAME_PAYLOAD_SIZE) // Payload 길이가 올바르지 않으면 빈 문자열을 반환합니다.
		return _T("");

	int wideChars = MultiByteToWideChar(CP_UTF8, 0, (LPCCH)frame.payload, frame.payload_len, NULL, 0); // UTF-8 Payload를 Unicode로 바꿀 때 필요한 문자 수를 계산합니다.
	if (wideChars <= 0) // UTF-8 변환에 실패하면 빈 문자열을 반환합니다.
		return _T("");

	CStringW wideText; // 복원된 Unicode 문자열을 저장합니다.
	LPWSTR buffer = wideText.GetBuffer(wideChars); // MultiByteToWideChar가 쓸 문자열 버퍼를 확보합니다.
	MultiByteToWideChar(CP_UTF8, 0, (LPCCH)frame.payload, frame.payload_len, buffer, wideChars); // Payload Byte를 Unicode 문자열로 복원합니다.
	wideText.ReleaseBuffer(wideChars); // CStringW 버퍼 길이를 실제 복원 길이로 확정합니다.
	return CString(wideText); // MFC 화면 출력용 CString으로 반환합니다.
}

void AppendPacketLog(CEdit& edit, LPCTSTR action, const Frame& frame, int packetBytes) // Frame 송수신 결과를 검증 가능한 로그로 출력합니다.
{
	CString payloadText = FramePayloadToText(frame); // 로그에 보여줄 Payload 문자열을 복원합니다.
	CString log; // 화면에 출력할 패킷 로그 문자열입니다.
	log.Format(_T("[%s PACKET] packet_bytes=%d seq=%d ack=%d checksum=%d payload_len=%d payload=\"%s\"\r\n"), action, packetBytes, frame.seq_num, frame.ack_num, frame.checksum, frame.payload_len, payloadText.GetString()); // Header 값과 Payload 정보를 포함한 로그를 만듭니다.
	AppendEditText(edit, log); // 지정한 출력창에 패킷 로그를 추가합니다.
}

UINT TXThread(LPVOID arg) // 송신 리스트의 메시지를 UDP로 전송하는 스레드 함수입니다.
{
	ThreadArg* pArg = (ThreadArg*)arg; // 스레드 인자를 ThreadArg 형식으로 변환합니다.
	CList<Frame, Frame&>* plist = pArg->pList; // 송신 Frame 패킷 리스트 주소를 가져옵니다.
	CUDPServerThdDlg* pDlg = (CUDPServerThdDlg*)pArg->pDlg; // 대화상자 주소를 가져옵니다.

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
			if (pDlg->m_hSocket != INVALID_SOCKET && !pDlg->m_clientAddr.IsEmpty()) // 소켓과 클라이언트 주소가 있을 때만 전송합니다.
			{
				SOCKADDR_IN client_addr; // 메시지를 보낼 클라이언트 주소 구조체입니다.
				memset(&client_addr, 0, sizeof(client_addr)); // 주소 구조체를 초기화합니다.
				client_addr.sin_family = AF_INET; // IPv4 주소 체계를 사용합니다.
				client_addr.sin_port = htons(pDlg->m_clientPort); // 클라이언트 포트 번호를 네트워크 바이트 순서로 저장합니다.
				InetPton(AF_INET, pDlg->m_clientAddr, &client_addr.sin_addr); // 클라이언트 IP 주소를 저장합니다.
				int sentBytes = sendto(pDlg->m_hSocket, (char*)&frame, sizeof(Frame), 0, (SOCKADDR*)&client_addr, sizeof(client_addr)); // Frame 구조체 전체를 UDP 패킷으로 보냅니다.
				if (sentBytes != SOCKET_ERROR) // 전송이 성공하면 패킷 로그를 남깁니다.
					AppendPacketLog(pDlg->m_packet_log_edit, _T("SEND"), frame, sentBytes);
				else // sendto 호출이 실패하면 전송 실패 로그를 남깁니다.
					AppendEditText(pDlg->m_packet_log_edit, _T("[SEND FAIL] sendto failed\r\n"));
			}
			else // 클라이언트 주소가 아직 없으면 전송 실패 로그를 남깁니다.
				AppendEditText(pDlg->m_packet_log_edit, _T("[SEND FAIL] client address is not ready\r\n"));
		}

		Sleep(10); // CPU 사용을 줄이기 위해 잠시 대기합니다.
	}

	return 0; // 스레드를 종료합니다.
}

UINT RXThread(LPVOID arg) // UDP로 받은 메시지를 화면에 출력하는 스레드 함수입니다.
{
	ThreadArg* pArg = (ThreadArg*)arg; // 스레드 인자를 ThreadArg 형식으로 변환합니다.
	CList<Frame, Frame&>* plist = pArg->pList; // 수신 Frame 패킷 리스트 주소를 가져옵니다.
	CUDPServerThdDlg* pDlg = (CUDPServerThdDlg*)pArg->pDlg; // 대화상자 주소를 가져옵니다.

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

			CString payloadText = FramePayloadToText(frame); // 수신한 Payload를 채팅창 출력 문자열로 복원합니다.
			payloadText += _T("\r\n"); // 채팅창에서 메시지 단위를 구분하기 위해 줄바꿈을 추가합니다.
			AppendEditText(pDlg->m_rx_edit, payloadText); // 복원된 채팅 메시지를 수신창에 출력합니다.
			AppendPacketLog(pDlg->m_packet_log_edit, _T("RECV"), frame, sizeof(Frame)); // 받은 Frame 정보를 패킷 로그창에 출력합니다.
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
	DDX_Control(pDX, IDC_PACKET_LOG, m_packet_log_edit); // 패킷 송수신 과정을 표시할 로그 컨트롤을 연결합니다.
}

BEGIN_MESSAGE_MAP(CUDPServerThdDlg, CDialogEx)
	ON_WM_SYSCOMMAND()
	ON_WM_PAINT()
	ON_WM_QUERYDRAGICON()
	ON_BN_CLICKED(IDC_SEND, &CUDPServerThdDlg::OnBnClickedSend)
	ON_BN_CLICKED(IDC_CLOSE, &CUDPServerThdDlg::OnBnClickedClose)
	ON_EN_CHANGE(IDC_EDIT1, &CUDPServerThdDlg::OnEnChangeEdit1)
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

	m_tx_edit_short.SetLimitText(FRAME_PAYLOAD_SIZE); // 영문 기준 16자 이상 입력되지 않도록 기본 제한을 설정합니다.

	CList<Frame, Frame&>* newlist = new CList<Frame, Frame&>; // 송신 Frame 패킷을 저장할 리스트를 생성합니다.
	arg1.pList = newlist; // 송신 스레드 인자에 송신 리스트를 저장합니다.
	arg1.Thread_run = 1; // 송신 스레드 실행 플래그를 켭니다.
	arg1.pDlg = this; // 송신 스레드 인자에 대화상자 주소를 저장합니다.

	CList<Frame, Frame&>* newlist2 = new CList<Frame, Frame&>; // 수신 Frame 패킷을 저장할 리스트를 생성합니다.
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
	Frame frame; // UDP에서 받은 패킷을 저장할 Frame 구조체입니다.
	CString PeerAddr; // 메시지를 보낸 클라이언트 IP 주소입니다.
	UINT PeerPort; // 메시지를 보낸 클라이언트 포트 번호입니다.
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

	TCHAR addrText[32]; // 보낸 클라이언트 IP 주소 문자열 버퍼입니다.
	InetNtop(AF_INET, &peer_addr.sin_addr, addrText, 32); // 보낸 클라이언트 IP 주소를 문자열로 변환합니다.
	PeerAddr = addrText; // 변환된 클라이언트 IP 주소를 저장합니다.
	PeerPort = ntohs(peer_addr.sin_port); // 보낸 클라이언트 포트 번호를 호스트 바이트 순서로 변환합니다.
	m_clientAddr = PeerAddr; // 마지막으로 메시지를 보낸 클라이언트 IP를 저장합니다.
	m_clientPort = PeerPort; // 마지막으로 메시지를 보낸 클라이언트 포트를 저장합니다.

	rx_cs.Lock(); // 수신 리스트 접근을 잠급니다.
	arg2.pList->AddTail(frame); // 받은 Frame 패킷을 수신 리스트에 추가합니다.
	rx_cs.Unlock(); // 수신 리스트 접근 잠금을 풉니다.
}

void CUDPServerThdDlg::OnBnClickedSend() // Send 버튼 클릭 시 메시지를 송신 리스트에 넣습니다.
{
	CString tx_message; // 사용자가 입력한 송신 메시지입니다.
	Frame frame; // 입력 메시지를 담을 Frame 패킷입니다.
	m_tx_edit_short.GetWindowTextW(tx_message); // 입력창의 문자열을 가져옵니다.

	if (!BuildFrameFromText(tx_message, frame)) // 입력 문자열이 Frame Payload 제한에 맞는지 확인합니다.
	{
		AppendEditText(m_packet_log_edit, _T("[SEND SKIP] payload must be 1-16 bytes\r\n")); // 전송하지 않은 이유를 패킷 로그에 남깁니다.
		m_tx_edit_short.SetFocus(); // 사용자가 바로 다시 입력할 수 있도록 포커스를 돌립니다.
		return;
	}

	CString tx_log = tx_message + _T("\r\n"); // 송신 채팅창에 표시할 문자열에 줄바꿈을 추가합니다.
	AppendEditText(m_tx_edit, tx_log); // 사용자가 보낸 원문 메시지를 송신창에 출력합니다.
	AppendPacketLog(m_packet_log_edit, _T("CREATE"), frame, sizeof(Frame)); // 생성된 Frame 정보를 패킷 로그창에 출력합니다.

	tx_cs.Lock(); // 송신 리스트 접근을 잠급니다.
	arg1.pList->AddTail(frame); // 송신할 Frame 패킷을 리스트에 추가합니다.
	tx_cs.Unlock(); // 송신 리스트 접근 잠금을 풉니다.

	m_tx_edit_short.SetWindowTextW(_T("")); // 입력창을 비웁니다.
	m_tx_edit_short.SetFocus(); // 입력창으로 포커스를 이동합니다.
}

void CUDPServerThdDlg::OnEnChangeEdit1() // 입력창의 UTF-8 Byte 수가 16Byte를 넘지 않도록 즉시 제한합니다.
{
	if (g_isTrimmingInput) // 코드가 입력창을 갱신하는 중이면 재진입을 막습니다.
		return;

	CString currentText; // 현재 입력창 문자열을 저장합니다.
	CString limitedText; // 16Byte 안에 들어가는 문자열만 저장합니다.
	m_tx_edit_short.GetWindowTextW(currentText); // 현재 입력창 문자열을 읽습니다.

	if (!LimitTextToPayloadSize(currentText, limitedText)) // 이미 16Byte 이하이면 수정하지 않습니다.
		return;

	g_isTrimmingInput = TRUE; // SetWindowTextW로 발생할 EN_CHANGE 재진입을 막습니다.
	m_tx_edit_short.SetWindowTextW(limitedText); // 16Byte를 넘는 부분을 제거한 문자열로 입력창을 갱신합니다.
	m_tx_edit_short.SetSel(limitedText.GetLength(), limitedText.GetLength()); // 커서를 제한된 문자열 끝으로 이동합니다.
	g_isTrimmingInput = FALSE; // 입력 제한 작업이 끝났음을 표시합니다.
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

