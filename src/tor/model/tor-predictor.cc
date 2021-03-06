
#include "ns3/log.h"
#include "ns3/random-variable-stream.h"
#include "ns3/point-to-point-net-device.h"

#include "tor-predictor.h"

#include <limits>

using namespace std;

namespace ns3 {

NS_LOG_COMPONENT_DEFINE ("TorPredApp");
NS_OBJECT_ENSURE_REGISTERED (TorPredApp);
NS_OBJECT_ENSURE_REGISTERED (PredCircuit);

TypeId
TorPredApp::GetTypeId (void)
{
  static TypeId tid = TypeId ("ns3::TorPredApp")
    .SetParent<TorBaseApp> ()
    .AddConstructor<TorPredApp> ()
    .AddAttribute ("FeedbackLoss", "Ratio of feedack messages that is lost randomly.",
                   DoubleValue (0.0),
                   MakeDoubleAccessor (&TorPredApp::m_feedback_loss),
                   MakeDoubleChecker<double> (0.0, 1.0))
    .AddAttribute ("OutOfBandFeedback", "Do not really send feedback messages over the network, but schedule their reception out of band.",
                   BooleanValue (false), // TODO
                   MakeBooleanAccessor (&TorPredApp::m_oob_feedback),
                   MakeBooleanChecker ())
    .AddTraceSource ("NewSocket",
                     "Trace indicating that a new socket has been installed.",
                     MakeTraceSourceAccessor (&TorPredApp::m_triggerNewSocket),
                     "ns3::TorPredApp::TorNewSocketCallback")
    .AddTraceSource ("NewServerSocket",
                     "Trace indicating that a new pseudo server socket has been installed.",
                     MakeTraceSourceAccessor (&TorPredApp::m_triggerNewPseudoServerSocket),
                     "ns3::TorPredApp::TorNewPseudoServerSocketCallback")
    .AddTraceSource ("BytesEnteredNetwork",
                     "Trace indicating that an exit sent a byte of data, identified by its index, into the network.",
                     MakeTraceSourceAccessor (&TorPredApp::m_triggerBytesEnteredNetwork),
                     "ns3::TorPredApp::TorBytesEnteredNetworkCallback")
    .AddTraceSource ("BytesLeftNetwork",
                     "Trace indicating that an eentry received a byte of data, identified by its index, from the network.",
                     MakeTraceSourceAccessor (&TorPredApp::m_triggerBytesLeftNetwork),
                     "ns3::TorPredApp::TorBytesLeftNetworkCallback");
  return tid;
}

TorPredApp::TorPredApp (void)
{
  listen_socket = 0;
  control_socket = 0;
  m_scheduleReadHead = 0;
  m_scheduleWriteHead = 0;
  controller = CreateObject<PredController> (this);
}

TorPredApp::~TorPredApp (void)
{
  NS_LOG_FUNCTION (this);
}

void
TorPredApp::DoDispose (void)
{
  NS_LOG_FUNCTION (this);
  listen_socket = 0;
  control_socket = 0;

  map<uint16_t,Ptr<PredCircuit> >::iterator i;
  for (i = circuits.begin (); i != circuits.end (); ++i)
    {
      i->second->DoDispose ();
    }
  circuits.clear ();
  baseCircuits.clear ();
  connections.clear ();
  Application::DoDispose ();
}


void
TorPredApp::AddCircuit (int id, Ipv4Address n_ip, int n_conntype, Ipv4Address p_ip, int p_conntype,
                    Ptr<PseudoClientSocket> clientSocket)
{
  TorBaseApp::AddCircuit (id, n_ip, n_conntype, p_ip, p_conntype);

  // ensure unique id
  NS_ASSERT (circuits[id] == 0);

  // allocate and init new circuit
  Ptr<PredConnection> p_conn = AddConnection (p_ip, p_conntype);
  Ptr<PredConnection> n_conn = AddConnection (n_ip, n_conntype);

  // set client socket of the predecessor connection, if one was given (default is 0)
  p_conn->SetSocket (clientSocket);
  m_triggerNewSocket(this, INBOUND, clientSocket);

  // inform controller about the connections (we only handle the incoming traffic)
  controller->AddInputConnection(n_conn);
  controller->AddOutputConnection(p_conn);

  Ptr<PredCircuit> circ = CreateObject<PredCircuit> (id, n_conn, p_conn);

  // add to circuit list maintained by every connection
  AddActiveCircuit (p_conn, circ);
  AddActiveCircuit (n_conn, circ);

  // add to the global list of circuits
  circuits[id] = circ;
  baseCircuits[id] = circ;
}

Ptr<PredConnection>
TorPredApp::AddConnection (Ipv4Address ip, int conntype)
{
  // Always create a new connection
  Ptr<PredConnection> conn = Create<PredConnection> (this, ip, conntype);
  connections.push_back (conn);

  return conn;
}

void
TorPredApp::AddActiveCircuit (Ptr<PredConnection> conn, Ptr<PredCircuit> circ)
{
  NS_ASSERT (conn);
  NS_ASSERT (circ);
  if (conn)
    {
      if (!conn->GetActiveCircuits ())
        {
          conn->SetActiveCircuits (circ);
          circ->SetNextCirc (conn, circ);
        }
      else
        {
          // insert in sorted order (sorted by circuit ID)
          Ptr<PredCircuit> first = conn->GetActiveCircuits();
          if (circ->GetId() <= first->GetId()) {
            circ->SetNextCirc (conn, first);
            conn->SetActiveCircuits (circ);

            // fix circular reference that still points to old "first" circuit
            Ptr<PredCircuit> current = first;
            while (current->GetNextCirc(conn) != first) {
              current = current->GetNextCirc(conn);
            }
            current->SetNextCirc(conn, circ);
          }
          else {
            // insert at a later position
            Ptr<PredCircuit> current = first;
            while (
              (current->GetNextCirc(conn)->GetId() <= circ->GetId()) &&
              (current->GetNextCirc(conn) != first)
            ) {
              current = current->GetNextCirc(conn);
            }
            // insert after current
            circ->SetNextCirc(conn, current->GetNextCirc(conn));
            current->SetNextCirc(conn, circ);
          }
        }
    }
}

void
TorPredApp::StartApplication (void)
{
  TorBaseApp::StartApplication ();
  m_readbucket.SetRefilledCallback (MakeCallback (&TorPredApp::RefillReadCallback, this));
  m_writebucket.SetRefilledCallback (MakeCallback (&TorPredApp::RefillWriteCallback, this));

  // create listen socket
  if (!listen_socket)
    {
      listen_socket = Socket::CreateSocket (GetNode (), TcpSocketFactory::GetTypeId ());
      listen_socket->Bind (m_local);
      listen_socket->Listen ();
      PredConnection::RememberName(Ipv4Address::ConvertFrom (m_ip), GetNodeName());
      cout << "remember " << Ipv4Address::ConvertFrom (m_ip) << " -> " << GetNodeName() << endl;
    }

  listen_socket->SetAcceptCallback (MakeNullCallback<bool,Ptr<Socket>,const Address &> (),
                                    MakeCallback (&TorPredApp::HandleAccept,this));

  // create control socket
  if (!control_socket)
    {
      control_socket = Socket::CreateSocket (GetNode (), TcpSocketFactory::GetTypeId ());
      control_socket->Bind (InetSocketAddress (Ipv4Address::GetAny (), 9002));
      control_socket->Listen ();
    }

  control_socket->SetAcceptCallback (MakeNullCallback<bool,Ptr<Socket>,const Address &> (),
                                    MakeCallback (&TorPredApp::HandleControlAccept,this));

  Ipv4Mask ipmask = Ipv4Mask ("255.0.0.0");

  // iterate over all neighboring connections
  vector<Ptr<PredConnection> >::iterator it;
  for ( it = connections.begin (); it != connections.end (); it++ )
    {
      Ptr<PredConnection> conn = *it;
      NS_ASSERT (conn);

      // remember the connection
      cout << "remembering connection (" << m_ip << " -> " << conn->GetRemote() << ")" << endl;
      PredConnection::RememberConnection(m_ip, conn->GetRemote(), conn);

      // if m_ip smaller then connect to remote node
      if (m_ip < conn->GetRemote () && conn->SpeaksCells ())
        {
          Ptr<Socket> socket = Socket::CreateSocket (GetNode (), TcpSocketFactory::GetTypeId ());
          socket->Bind ();
          socket->Connect (Address (InetSocketAddress (conn->GetRemote (), InetSocketAddress::ConvertFrom (m_local).GetPort ())));
          socket->SetSendCallback (MakeCallback(&TorPredApp::ConnWriteCallback, this));
          // socket->SetDataSentCallback (MakeCallback (&TorPredApp::ConnWriteCallback, this));
          socket->SetRecvCallback (MakeCallback (&TorPredApp::ConnReadCallback, this));
          conn->SetSocket (socket);
          m_triggerNewSocket(this, OUTBOUND, socket);

          // Remember our local connection object
          PredConnection::RememberLocalConnection(socket, conn);

          Ptr<Socket> control_socket = Socket::CreateSocket (GetNode (), TcpSocketFactory::GetTypeId ());
          control_socket->Bind ();
          control_socket->Connect (Address (InetSocketAddress (conn->GetRemote (), 9002)));
          control_socket->SetRecvCallback (MakeCallback (&TorPredApp::ConnReadControlCallback, this));
          conn->SetControlSocket (control_socket);
        }

      if (ipmask.IsMatch (conn->GetRemote (), Ipv4Address ("127.0.0.1")) )
        {
          if (conn->GetType () == SERVEREDGE)
            {
              Ptr<Socket> socket = CreateObject<PseudoServerSocket> ();
              // socket->SetDataSentCallback (MakeCallback (&TorPredApp::ConnWriteCallback, this));
              socket->SetSendCallback(MakeCallback(&TorPredApp::ConnWriteCallback, this));
              socket->SetRecvCallback (MakeCallback (&TorPredApp::ConnReadCallback, this));
              conn->SetSocket (socket);
              m_triggerNewSocket(this, OUTBOUND, socket);

              int circId = conn->GetActiveCircuits () ->GetId ();
              m_triggerNewPseudoServerSocket(this, circId, DynamicCast<PseudoServerSocket>(socket));
            }

          if (conn->GetType () == PROXYEDGE)
            {
              Ptr<Socket> socket = conn->GetSocket ();
              // Create a default pseudo client if none was previously provided via AddCircuit().
              // Normally, this should not happen (a specific pseudo socket should always be
              // provided for a proxy connection).
              if (!socket)
                {
                  socket = CreateObject<PseudoClientSocket> ();
                  m_triggerNewSocket(this, INBOUND, socket);
                }

              // socket->SetDataSentCallback (MakeCallback (&TorPredApp::ConnWriteCallback, this));
              socket->SetSendCallback(MakeCallback(&TorPredApp::ConnWriteCallback, this));
              socket->SetRecvCallback (MakeCallback (&TorPredApp::ConnReadCallback, this));
              conn->SetSocket (socket);
            }
        }
    }
  
  controller->Setup (); 

  m_triggerAppStart (Ptr<TorBaseApp>(this));
  NS_LOG_INFO ("StartApplication " << m_name << " ip=" << m_ip);
}

void
TorPredApp::StopApplication (void)
{
  // close listen socket
  if (listen_socket)
    {
      listen_socket->Close ();
      listen_socket->SetRecvCallback (MakeNullCallback<void, Ptr<Socket> > ());
    }

  // close control socket
  if (control_socket)
    {
      control_socket->Close ();
      control_socket->SetRecvCallback (MakeNullCallback<void, Ptr<Socket> > ());
    }

  // close all connections
  vector<Ptr<PredConnection> >::iterator it_conn;
  for ( it_conn = connections.begin (); it_conn != connections.end (); ++it_conn )
    {
      Ptr<PredConnection> conn = *it_conn;
      NS_ASSERT (conn);
      if (conn->GetSocket ())
        {
          conn->GetSocket ()->Close ();
          conn->GetSocket ()->SetRecvCallback (MakeNullCallback<void, Ptr<Socket> > ());
          conn->GetSocket ()->SetDataSentCallback (MakeNullCallback<void, Ptr<Socket>, uint32_t > ());
          // conn->GetSocket()->SetSendCallback(MakeNullCallback<void, Ptr<Socket>, uint32_t > ());
        }
    }
}

Ptr<PredCircuit>
TorPredApp::GetCircuit (uint16_t circid)
{
  return circuits[circid];
}

void
TorPredApp::ConnReadControlCallback (Ptr<Socket> socket)
{
  NS_ASSERT (socket);
  Ptr<PredConnection> conn = LookupConnByControlSocket (socket);
  NS_ASSERT (conn);
  NS_ASSERT (conn->SpeaksCells ());

  vector<Ptr<Packet>> packet_list;
  conn->ReadControl (packet_list);

  for (auto& cell : packet_list)
  {
    NS_ASSERT (IsDirectCell (cell));
    HandleDirectCell(conn, cell);
  }
}

void
TorPredApp::ConnReadCallback (Ptr<Socket> socket)
{
  // At one of the connections, data is ready to be read. This does not mean
  // the data has been read already, but it has arrived in the RX buffer. We
  // decide whether we want to read it or not (depending on whether the
  // connection is blocked).

  NS_ASSERT (socket);
  Ptr<PredConnection> conn = LookupConn (socket);
  NS_ASSERT (conn);

  uint32_t base = conn->SpeaksCells () ? CELL_NETWORK_SIZE : CELL_PAYLOAD_SIZE;
  uint32_t max_read = RoundRobin (base, m_readbucket.GetSize ());

  // find the minimum amount of data to read safely from the socket
  max_read = min (max_read, socket->GetRxAvailable ());
  NS_LOG_LOGIC ("[" << GetNodeName() << ": connection " << conn->GetRemoteName () << "] Reading " << max_read << "/" << socket->GetRxAvailable () << " bytes");

  if (m_readbucket.GetSize() <= 0 && m_scheduleReadHead == 0)
    {
      m_scheduleReadHead = conn;
    }
  
  // For exits, limit the amount of data that can be read from pseudo server sockets
  // by applying the per-connection read bucket based on the previously predicted
  // v_in value.

  // Find out if this is an outgoing connection according to the optimization problem
  bool is_inconn = controller->IsInConn(conn);
  std::function<void(int)> decrement_per_conn_bucket;

  if (is_inconn && !conn->SpeaksCells ())
  {
    NS_ASSERT(controller->conn_read_buckets.find(conn) != controller->conn_read_buckets.end());
    uint64_t& conn_read_bucket = controller->conn_read_buckets[conn];
    decrement_per_conn_bucket = [&conn_read_bucket] (int diff) { conn_read_bucket -= diff; };

    max_read = (uint32_t) std::min((uint64_t)max_read, conn_read_bucket);
  }
  else
  {
    // remember to not decrement the bucket later (there is none)
    decrement_per_conn_bucket = [] (int) { };
  }

  if (max_read <= 0)
    {
      return;
    }

  vector<Ptr<Packet> > packet_list;
  uint32_t read_bytes = conn->Read (&packet_list, max_read);

  NS_LOG_LOGIC ("[" << GetNodeName() << ": connection " << conn->GetRemoteName () << "] Got " << packet_list.size () << " packets (read " << read_bytes << " bytes)");

  for (uint32_t i = 0; i < packet_list.size (); i++)
    {
      if (conn->SpeaksCells ())
        {
          NS_LOG_LOGIC ("[" << GetNodeName() << ": connection " << conn->GetRemoteName () << "] Handling relay cell...");
          ReceiveRelayCell (conn, packet_list[i]);
        }
      else
        {
          NS_LOG_LOGIC ("[" << GetNodeName() << ": connection " << conn->GetRemoteName () << "] Handling non-relay cell, packaging it...");
          PackageRelayCell (conn, packet_list[i]);
        }
    }

  if (read_bytes > 0)
    {
      // decrement buckets
      GlobalBucketsDecrement (read_bytes, 0);
      conn->m_data_received += read_bytes;
      decrement_per_conn_bucket((int) read_bytes);

      // try to read more
      if (socket->GetRxAvailable () > 0)
        {
          // add some virtual processing time before reading more
          Time delay = Time::FromInteger (read_bytes * 2, Time::NS);
          conn->ScheduleRead (delay);
        }
    }
}

void
TorPredApp::PackageRelayCell (Ptr<PredConnection> conn, Ptr<Packet> cell)
{
  NS_ASSERT (conn);
  NS_ASSERT (cell);
  Ptr<PredCircuit> circ = conn->GetActiveCircuits ();
  NS_ASSERT (circ);

  PackageRelayCellImpl (circ->GetId (), cell);

  CellDirection direction = circ->GetOppositeDirection (conn);
  AppendCellToCircuitQueue (circ, cell, direction);
  NS_LOG_LOGIC ("[" << GetNodeName() << ": circuit " << circ->GetId () << "] Appended newly packaged cell to circ queue.");
}

void
TorPredApp::PackageRelayCellImpl (uint16_t circ_id, Ptr<Packet> cell)
{
  NS_ASSERT (cell);
  CellHeader h;
  h.SetCircId (circ_id);
  h.SetCmd (RELAY_DATA);
  h.SetType (RELAY);
  h.SetLength (cell->GetSize ());
  cell->AddHeader (h);
}

void
TorPredApp::ReceiveRelayCell (Ptr<PredConnection> conn, Ptr<Packet> cell)
{
  NS_ASSERT (conn);
  NS_ASSERT (cell);

  NS_LOG_LOGIC ("[" << GetNodeName() << ": connection " << conn->GetRemoteName () << "] received relay cell (" << (IsDirectCell(cell) ? "connection" : "circuit") << "-level)");

  NS_ASSERT (!IsDirectCell(cell));

  Ptr<PredCircuit> circ = LookupCircuitFromCell (cell);
  NS_ASSERT (circ);
  //NS_LOG_LOGIC ("[" << GetNodeName() << ": connection " << Ipv4Address::ConvertFrom (conn->GetRemote()) << "/" << conn->GetRemoteName () << "] received relay cell.");

  // find target connection for relaying
  CellDirection direction = circ->GetOppositeDirection (conn);
  Ptr<PredConnection> target_conn = circ->GetConnection (direction);
  NS_ASSERT (target_conn);
  target_conn->CountFinalReception(circ->GetId(), cell->GetSize());

  AppendCellToCircuitQueue (circ, cell, direction);
}

bool
TorPredApp::IsDirectCell (Ptr<Packet> cell)
{
  if (!cell)
  {
    return false;
  }

  CellHeader h;
  cell->PeekHeader (h);

  return (h.GetType() == DIRECT_MULTI || h.GetType() == DIRECT_MULTI_END);
}

void
TorPredApp::HandleDirectCell (Ptr<PredConnection> conn, Ptr<Packet> cell)
{
  auto& decoder = multicell_decoders[conn];
  
  decoder.AddCell(cell);
  if (decoder.IsReady())
  {
    Ptr<Packet> multicell = decoder.GetAndReset();
    controller->HandleFeedback(conn, multicell);
  }
}


Ptr<PredCircuit>
TorPredApp::LookupCircuitFromCell (Ptr<Packet> cell)
{
  NS_ASSERT (cell);
  CellHeader h;
  cell->PeekHeader (h);
  return circuits[h.GetCircId ()];
}


/* Add cell to the queue of circ writing to orconn transmitting in direction. */
void
TorPredApp::AppendCellToCircuitQueue (Ptr<PredCircuit> circ, Ptr<Packet> cell, CellDirection direction)
{
  NS_ASSERT (circ);
  NS_ASSERT (cell);
  queue<Ptr<Packet> > *queue = circ->GetQueue (direction);
  Ptr<PredConnection> conn = circ->GetConnection (direction);
  NS_ASSERT (queue);
  NS_ASSERT (conn);

  circ->PushCell (cell, direction);

  NS_LOG_LOGIC ("[" << GetNodeName() << ": circuit " << circ->GetId () << "] Appended cell. Queue holds " << queue->size () << " cells.");
  conn->ScheduleWrite ();
}


void
TorPredApp::ConnWriteCallback (Ptr<Socket> socket, uint32_t tx)
{
  NS_ASSERT (socket);
  Ptr<PredConnection> conn = LookupConn (socket);
  NS_ASSERT (conn);

  uint32_t newtx = socket->GetTxAvailable ();
  NS_LOG_LOGIC ("[" << GetNodeName() << ": connection " << conn->GetRemoteName () << "] newtx = " << (int) newtx);
  
  uint32_t bucket_allowed = m_writebucket.GetSize ();
  NS_LOG_LOGIC ("[" << GetNodeName() << ": connection " << conn->GetRemoteName () << "] bucket_allowed = " << (int) bucket_allowed);

  // Find out if this is an outgoing connection according to the optimization problem
  bool is_outconn = controller->IsOutConn(conn);
  
  if (is_outconn)
  {
    dumper.dump("connlevel-bucket-before-write",
                  "time", Simulator::Now().GetSeconds(),
                  "node", GetNodeName(),
                  "conn", conn->GetRemoteName(),
                  "bytes", (int) controller->conn_buckets[conn]
    );
  }

  // Additionally, limit the allowed sending data size/rate by the per-connection
  // token bucket, realizing the throttling defined by v_out.
  std::function<void(int)> decrement_per_conn_bucket;
  if (is_outconn)
  {
    NS_ASSERT(controller->conn_buckets.find(conn) != controller->conn_buckets.end());
    uint64_t& conn_bucket = controller->conn_buckets[conn];
    decrement_per_conn_bucket = [&conn_bucket] (int diff) { conn_bucket -= diff; };

    NS_LOG_LOGIC ("[" << GetNodeName() << ": connection " << conn->GetRemoteName () << "] Applying per-connection token bucket");
    bucket_allowed = (uint32_t) std::min((uint64_t)bucket_allowed, conn_bucket);
  }
  else
  {
    // remember to not decrement the bucket later (there is none)
    decrement_per_conn_bucket = [] (int) { };
  }
  NS_LOG_LOGIC ("[" << GetNodeName() << ": connection " << conn->GetRemoteName () << "] bucket_allowed (incl. connlevel) = " << (int) bucket_allowed);


  int written_bytes = 0;
  uint32_t base = conn->SpeaksCells () ? CELL_NETWORK_SIZE : CELL_PAYLOAD_SIZE;
  uint32_t max_write = RoundRobin (base, bucket_allowed);
  NS_LOG_LOGIC ("[" << GetNodeName() << ": connection " << conn->GetRemoteName () << "] max_write = " << (int) max_write);
  
  if (max_write > newtx)
  {
    dumper.dump("send-limited-by-txavail",
                  "time", Simulator::Now().GetSeconds(),
                  "node", GetNodeName(),
                  "conn", conn->GetRemoteName(),
                  "bytes", (int) (max_write-newtx)
    );
  }
  dumper.dump("send-txavail-diff",
                "time", Simulator::Now().GetSeconds(),
                "node", GetNodeName(),
                "conn", conn->GetRemoteName(),
                "bytes", (int) max_write - (int) newtx
  );

  max_write = max_write > newtx ? newtx : max_write;

  NS_LOG_LOGIC ("[" << GetNodeName() << ": connection " << conn->GetRemoteName () << "] writing at most " << max_write << " bytes into Conn");

  if (m_writebucket.GetSize() <= 0 && m_scheduleWriteHead == 0) {
    m_scheduleWriteHead = conn;
  }

  if (max_write <= 0)
    {
      return;
    }

  written_bytes = conn->Write (max_write);
  NS_LOG_LOGIC ("[" << GetNodeName() << ": connection " << conn->GetRemoteName () << "] " << written_bytes << " bytes written");

  if (written_bytes > 0)
    {
      GlobalBucketsDecrement (0, written_bytes);
      decrement_per_conn_bucket(written_bytes);
      conn->m_data_sent += written_bytes;

      /* try flushing more */
      conn->ScheduleWrite ();
    }

  if (is_outconn)
  {
    dumper.dump("connlevel-bucket-after-write",
                  "time", Simulator::Now().GetSeconds(),
                  "node", GetNodeName(),
                  "conn", conn->GetRemoteName(),
                  "bytes", (int) controller->conn_buckets[conn]
    );
  }
}



void
TorPredApp::HandleAccept (Ptr<Socket> s, const Address& from)
{
  Ptr<PredConnection> conn;
  Ipv4Address ip = InetSocketAddress::ConvertFrom (from).GetIpv4 ();
  vector<Ptr<PredConnection> >::iterator it;
  for (it = connections.begin (); it != connections.end (); ++it)
    {
      // Since we require here that the connection be not established yet,
      // this works with multiple connections from the same remote IP.
      if ((*it)->GetRemote () == ip && !(*it)->GetSocket () )
        {
          conn = *it;
          break;
        }
    }

  NS_ASSERT (conn);
  conn->SetSocket (s);
  m_triggerNewSocket(this, INBOUND, s);

  PredConnection::RememberLocalConnection(s, conn);

  s->SetRecvCallback (MakeCallback (&TorPredApp::ConnReadCallback, this));
  s->SetSendCallback (MakeCallback(&TorPredApp::ConnWriteCallback, this));
  // s->SetDataSentCallback (MakeCallback (&TorPredApp::ConnWriteCallback, this));
  conn->ScheduleWrite();
  conn->ScheduleRead();
}

void
TorPredApp::HandleControlAccept (Ptr<Socket> s, const Address& from)
{
  Ptr<PredConnection> conn;
  Ipv4Address ip = InetSocketAddress::ConvertFrom (from).GetIpv4 ();
  vector<Ptr<PredConnection> >::iterator it;
  for (it = connections.begin (); it != connections.end (); ++it)
    {
      if ((*it)->GetRemote () == ip && !(*it)->GetControlSocket () )
        {
          conn = *it;
          break;
        }
    }

  NS_ASSERT (conn);
  conn->SetControlSocket (s);

  s->SetRecvCallback (MakeCallback (&TorPredApp::ConnReadControlCallback, this));

  controller->Start();
}



Ptr<PredConnection>
TorPredApp::LookupConn (Ptr<Socket> socket)
{
  vector<Ptr<PredConnection> >::iterator it;
  for ( it = connections.begin (); it != connections.end (); it++ )
    {
      NS_ASSERT (*it);
      if ((*it)->GetSocket () == socket)
        {
          return (*it);
        }
    }
  return NULL;
}

Ptr<PredConnection>
TorPredApp::LookupConnByControlSocket (Ptr<Socket> socket)
{
  vector<Ptr<PredConnection> >::iterator it;
  for ( it = connections.begin (); it != connections.end (); it++ )
    {
      NS_ASSERT (*it);
      if ((*it)->GetControlSocket () == socket)
        {
          return (*it);
        }
    }
  return NULL;
}


void
TorPredApp::RefillReadCallback (int64_t prev_read_bucket)
{
  NS_LOG_LOGIC ("[" << GetNodeName() << "] " << "read bucket was " << prev_read_bucket << ". Now " << m_readbucket.GetSize ());
  if (prev_read_bucket <= 0 && m_readbucket.GetSize () > 0)
    {
      vector<Ptr<PredConnection> >::iterator it;
      vector<Ptr<PredConnection> >::iterator headit;

      headit = connections.begin();
      if (m_scheduleReadHead != 0) {
        headit = find(connections.begin(),connections.end(),m_scheduleReadHead);
        m_scheduleReadHead = 0;
      }

      it = headit;
      do {
        Ptr<PredConnection> conn = *it;
        NS_ASSERT(conn);
        conn->ScheduleRead (Time ("10ns"));
        if (++it == connections.end ()) {
          it = connections.begin ();
        }
      } while (it != headit);
    }
}

void
TorPredApp::RefillWriteCallback (int64_t prev_write_bucket)
{
  NS_LOG_LOGIC ("[" << GetNodeName() << "] " << "write bucket was " << prev_write_bucket << ". Now " << m_writebucket.GetSize ());

  if (prev_write_bucket <= 0 && m_writebucket.GetSize () > 0)
    {
      vector<Ptr<PredConnection> >::iterator it;
      vector<Ptr<PredConnection> >::iterator headit;

      headit = connections.begin();
      if (m_scheduleWriteHead != 0) {
        headit = find(connections.begin(),connections.end(),m_scheduleWriteHead);
        m_scheduleWriteHead = 0;
      }

      it = headit;
      do {
        Ptr<PredConnection> conn = *it;
        NS_ASSERT(conn);
        conn->ScheduleWrite ();
        if (++it == connections.end ()) {
          it = connections.begin ();
        }
      } while (it != headit);
    }
}


/** We just read num_read and wrote num_written bytes
 * onto conn. Decrement buckets appropriately. */
void
TorPredApp::GlobalBucketsDecrement (uint32_t num_read, uint32_t num_written)
{
  m_readbucket.Decrement (num_read);
  m_writebucket.Decrement (num_written);
}



/** Helper function to decide how many bytes out of global_bucket
 * we're willing to use for this transaction. Yes, this is how Tor
 * implements it; no kidding. */
uint32_t
TorPredApp::RoundRobin (int base, int64_t global_bucket)
{
  uint32_t num_bytes_high = 32 * base;
  uint32_t num_bytes_low = 4 * base;
  int64_t at_most = global_bucket / 8;
  at_most -= (at_most % base);

  if (at_most > num_bytes_high)
    {
      at_most = num_bytes_high;
    }
  else if (at_most < num_bytes_low)
    {
      at_most = num_bytes_low;
    }

  if (at_most > global_bucket)
    {
      at_most = global_bucket;
    }

  if (at_most < 0)
    {
      return 0;
    }
  return at_most;
}



PredCircuit::PredCircuit (uint16_t circ_id, Ptr<PredConnection> n_conn, Ptr<PredConnection> p_conn) : BaseCircuit (circ_id)
{
  this->p_cellQ = new queue<Ptr<Packet> >;
  this->n_cellQ = new queue<Ptr<Packet> >;

  this->p_conn = p_conn;
  this->n_conn = n_conn;

  this->next_active_on_n_conn = 0;
  this->next_active_on_p_conn = 0;
}


PredCircuit::~PredCircuit ()
{
  NS_LOG_FUNCTION (this);
  delete this->p_cellQ;
  delete this->n_cellQ;
}

void
PredCircuit::DoDispose ()
{
  this->next_active_on_p_conn = 0;
  this->next_active_on_n_conn = 0;
  this->p_conn->SetActiveCircuits (0);
  this->n_conn->SetActiveCircuits (0);
}


Ptr<Packet>
PredCircuit::PopQueue (queue<Ptr<Packet> > *queue)
{
  if (queue->size () > 0)
    {
      Ptr<Packet> cell = queue->front ();
      queue->pop ();

      return cell;
    }
  return 0;
}


Ptr<Packet>
PredCircuit::PopCell (CellDirection direction)
{
  Ptr<Packet> cell;
  if (direction == OUTBOUND)
    {
      cell = this->PopQueue (this->n_cellQ);
    }
  else
    {
      cell = this->PopQueue (this->p_cellQ);
    }

  if (cell)
    {
      // TODO: only if not a special cell?
      IncrementStats (direction, 0, CELL_PAYLOAD_SIZE);
    }

  return cell;
}


void
PredCircuit::PushCell (Ptr<Packet> cell, CellDirection direction)
{
  if (cell)
    {
      Ptr<PredConnection> conn = GetConnection (direction);
      Ptr<PredConnection> opp_conn = GetOppositeConnection (direction);

      if (!conn->SpeaksCells ())
        {
          // delivery

          CellHeader h;
          cell->RemoveHeader (h);
        }

      IncrementStats (direction, CELL_PAYLOAD_SIZE, 0);
      GetQueue (direction)->push (cell);
    }
}





Ptr<PredConnection>
PredCircuit::GetConnection (CellDirection direction)
{
  if (direction == OUTBOUND)
    {
      return this->n_conn;
    }
  else
    {
      return this->p_conn;
    }
}

Ptr<PredConnection>
PredCircuit::GetOppositeConnection (CellDirection direction)
{
  if (direction == OUTBOUND)
    {
      return this->p_conn;
    }
  else
    {
      return this->n_conn;
    }
}

Ptr<PredConnection>
PredCircuit::GetOppositeConnection (Ptr<PredConnection> conn)
{
  if (this->n_conn == conn)
    {
      return this->p_conn;
    }
  else if (this->p_conn == conn)
    {
      return this->n_conn;
    }
  else
    {
      return 0;
    }
}



CellDirection
PredCircuit::GetDirection (Ptr<PredConnection> conn)
{
  if (this->n_conn == conn)
    {
      return OUTBOUND;
    }
  else
    {
      return INBOUND;
    }
}

CellDirection
PredCircuit::GetOppositeDirection (Ptr<PredConnection> conn)
{
  if (this->n_conn == conn)
    {
      return INBOUND;
    }
  else
    {
      return OUTBOUND;
    }
}

Ptr<PredCircuit>
PredCircuit::GetNextCirc (Ptr<PredConnection> conn)
{
  NS_ASSERT (this->n_conn);
  if (this->n_conn == conn)
    {
      return next_active_on_n_conn;
    }
  else
    {
      return next_active_on_p_conn;
    }
}


void
PredCircuit::SetNextCirc (Ptr<PredConnection> conn, Ptr<PredCircuit> circ)
{
  if (this->n_conn == conn)
    {
      next_active_on_n_conn = circ;
    }
  else
    {
      next_active_on_p_conn = circ;
    }
}


queue<Ptr<Packet> >*
PredCircuit::GetQueue (CellDirection direction)
{
  if (direction == OUTBOUND)
    {
      return this->n_cellQ;
    }
  else
    {
      return this->p_cellQ;
    }
}


uint32_t
PredCircuit::GetQueueSize (CellDirection direction)
{
  if (direction == OUTBOUND)
    {
      return this->n_cellQ->size ();
    }
  else
    {
      return this->p_cellQ->size ();
    }
}

uint32_t
PredCircuit::GetQueueSizeBytes (CellDirection direction)
{
  queue<Ptr<Packet>> * source = (direction == OUTBOUND) ? this->n_cellQ : this->p_cellQ;
  
  // copy temporarily
  queue<Ptr<Packet>> q{*source};

  uint32_t result = 0;
  while (q.size() > 0)
  {
    result += q.front()->GetSize();
    q.pop();
  }
  return result;
}








PredConnection::PredConnection (TorPredApp* torapp, Ipv4Address ip, int conntype)
{
  this->torapp = torapp;
  this->remote = ip;
  this->inbuf.size = 0;
  this->outbuf.size = 0;
  this->active_circuits = 0;

  m_socket = 0;
  m_conntype = conntype;
  m_data_sent = 0;
  m_data_received = 0;
}


PredConnection::~PredConnection ()
{
  NS_LOG_FUNCTION (this);
}

Ptr<PredCircuit>
PredConnection::GetActiveCircuits ()
{
  return active_circuits;
}

void
PredConnection::SetActiveCircuits (Ptr<PredCircuit> circ)
{
  active_circuits = circ;
}


uint8_t
PredConnection::GetType ()
{
  return m_conntype;
}

bool
PredConnection::SpeaksCells ()
{
  return m_conntype == RELAYEDGE;
}


Ptr<Socket>
PredConnection::GetSocket ()
{
  return m_socket;
}

Ptr<Socket>
PredConnection::GetControlSocket ()
{
  return m_controlsocket;
}

void
PredConnection::SetSocket (Ptr<Socket> socket)
{
  // called both when establishing the connection and when accepting it
  m_socket = socket;
}

void
PredConnection::SetControlSocket (Ptr<Socket> socket)
{
  // called both when establishing the connection and when accepting it
  m_controlsocket = socket;
}

Time
PredConnection::GetBaseRtt ()
{
  return torapp->GetPeerDelay (GetRemote ());
}

Ipv4Address
PredConnection::GetRemote ()
{
  return remote;
}

map<Ipv4Address,string> PredConnection::remote_names;
map<pair<Ipv4Address,Ipv4Address>, Ptr<PredConnection>> PredConnection::remote_connections;
map<pair<InetSocketAddress,InetSocketAddress>, Ptr<PredConnection>> PredConnection::local_connections;

void
PredConnection::RememberName (Ipv4Address address, string name)
{
  PredConnection::remote_names[address] = name;
}

void
PredConnection::RememberConnection (Ipv4Address from, Ipv4Address to, Ptr<PredConnection> conn)
{
  PredConnection::remote_connections[make_pair(from, to)] = conn;
}

void
PredConnection::RememberLocalConnection (Ptr<Socket> socket, Ptr<PredConnection> conn)
{
  ns3::Address local_raw_addr;
  socket->GetSockName (local_raw_addr);
  NS_ASSERT (!local_raw_addr.IsInvalid());
  auto local_addr = InetSocketAddress::ConvertFrom (local_raw_addr);

  ns3::Address remote_raw_addr;
  socket->GetPeerName (remote_raw_addr);
  NS_ASSERT (!remote_raw_addr.IsInvalid());
  auto remote_addr = InetSocketAddress::ConvertFrom (remote_raw_addr);

  auto key = std::make_pair(local_addr, remote_addr);
  NS_ASSERT (local_connections.find(key) == local_connections.end());
  local_connections[key] = conn;
}

Ptr<PredConnection>
PredConnection::GetRemoteConn ()
{
  Ptr<Socket> socket = GetSocket();

  ns3::Address local_raw_addr;
  socket->GetPeerName (local_raw_addr); // Reversed direction!
  NS_ASSERT (!local_raw_addr.IsInvalid());
  auto local_addr = InetSocketAddress::ConvertFrom (local_raw_addr);

  ns3::Address remote_raw_addr;
  socket->GetSockName (remote_raw_addr); // Reversed direction!
  NS_ASSERT (!remote_raw_addr.IsInvalid());
  auto remote_addr = InetSocketAddress::ConvertFrom (remote_raw_addr);

  auto key = std::make_pair(local_addr, remote_addr);
  
  if (local_connections.find(key) == local_connections.end())
  {
    NS_LOG_LOGIC ("[" << torapp->GetNodeName() << "/" << GetRemoteName() << "] cannot get remote connection (yet)");
    return nullptr;
  }
  
  return local_connections[key];
}

string
PredConnection::GetRemoteName ()
{
  if (Ipv4Mask ("255.0.0.0").IsMatch (GetRemote (), Ipv4Address ("127.0.0.1")) )
  {
    stringstream ss;
    // GetRemote().Print(ss);
    ss << GetActiveCircuits()->GetId();
    if(DynamicCast<PseudoServerSocket>(GetSocket()))
    {
      ss << "-server";
    }
    else
    {
      NS_ASSERT(DynamicCast<PseudoClientSocket>(GetSocket()));
      ss << "-client";
    }
    
    return string("pseudo-") + ss.str();
  }

  map<Ipv4Address,string>::const_iterator it = PredConnection::remote_names.find(GetRemote ());
  NS_ASSERT(it != PredConnection::remote_names.end() );
  string name{it->second};

  // Add the circuit ID to the connection name
  auto circs = GetAllActiveCircuits();
  NS_ASSERT_MSG (circs.size() == 1, "connection does not have exactly one circuit");
  stringstream result;
  result << name << "." << circs[0]->GetId();

  return result.str();
}



uint32_t
PredConnection::Read (vector<Ptr<Packet> >* packet_list, uint32_t max_read)
{
  uint8_t raw_data[max_read + this->inbuf.size];
  memcpy (raw_data, this->inbuf.data, this->inbuf.size);
  uint32_t tmp_available = m_socket->GetRxAvailable ();
  int read_bytes = m_socket->Recv (&raw_data[this->inbuf.size], max_read, 0);

  uint32_t base = SpeaksCells () ? CELL_NETWORK_SIZE : CELL_PAYLOAD_SIZE;
  uint32_t datasize = read_bytes + inbuf.size;
  uint32_t leftover = datasize % base;
  int num_packages = datasize / base;

  NS_LOG_LOGIC ("[" << torapp->GetNodeName() << ": connection " << GetRemoteName () << "] " <<
      "GetRxAvailable = " << tmp_available << ", "
      "max_read = " << max_read << ", "
      "read_bytes = " << read_bytes << ", "
      "base = " << base << ", "
      "datasize = " << datasize << ", "
      "leftover = " << leftover << ", "
      "num_packages = " << num_packages
  );

  // slice data into packets
  Ptr<Packet> cell;
  for (int i = 0; i < num_packages; i++)
    {
      cell = Create<Packet> (&raw_data[i * base], base);
      packet_list->push_back (cell);
    }

  //safe leftover
  memcpy (inbuf.data, &raw_data[datasize - leftover], leftover);
  inbuf.size = leftover;

  return read_bytes;
}


uint32_t
PredConnection::ReadControl (vector<Ptr<Packet>>& packet_list)
{
  uint32_t available = m_controlsocket->GetRxAvailable();
  if (available == 0)
  {
    return 0;
  }

  // make room for all the data that is available from the socket
  size_t old_bufsize = control_inbuf.size();
  control_inbuf.resize(old_bufsize + available);

  // read the data into our buffer
  int read_bytes = m_controlsocket->Recv (
    control_inbuf.data() + old_bufsize,
    available,
    0
  );

  // determine number of complete cells
  NS_ASSERT(SpeaksCells());
  const uint32_t base = CELL_NETWORK_SIZE;
  size_t num_packets = control_inbuf.size() / base;

  // slice data into packets
  for (size_t i=0; i<num_packets; i++)
  {
    Ptr<Packet> cell = Create<Packet> (control_inbuf.data() + i*base, base);
    packet_list.push_back (cell);
  }

  // remove read data, but keep potential leftover
  control_inbuf.erase(control_inbuf.begin(), control_inbuf.begin() + num_packets*base);

  return read_bytes;
}

void
PredConnection::CountFinalReception(int circid, uint32_t length)
{
  if (!SpeaksCells() && DynamicCast<PseudoClientSocket>(GetSocket()))
  {
    // notify about bytes leaving the network
    torapp->m_triggerBytesLeftNetwork(torapp, circid, data_index_last_delivered[circid] + 1, data_index_last_delivered[circid] + (uint64_t)length);
    data_index_last_delivered[circid] += (uint64_t) length;
  }
}

uint32_t
PredConnection::Write (uint32_t max_write)
{
  uint32_t base = SpeaksCells () ? CELL_NETWORK_SIZE : CELL_PAYLOAD_SIZE;
  uint8_t raw_data[outbuf.size + (max_write / base + 1) * base];
  memcpy (raw_data, outbuf.data, outbuf.size);
  uint32_t datasize = outbuf.size;
  int written_bytes = 0;

  // fill raw_data
  bool flushed_some = false;
  Ptr<PredCircuit> start_circ = GetActiveCircuits ();
  NS_ASSERT (start_circ);
  Ptr<PredCircuit> circ;
  Ptr<Packet> cell = Ptr<Packet> (NULL);
  CellDirection direction;

  NS_LOG_LOGIC ("[" << torapp->GetNodeName () << ": connection " << GetRemoteName () << "] Trying to write cells from circuit queues");

  while (datasize < max_write)
    {
      circ = GetActiveCircuits ();
      NS_ASSERT (circ);
      int circid = circ->GetId();

      direction = circ->GetDirection (this);
      cell = circ->PopCell (direction);

      if (cell)
      {
        uint32_t cell_size = cell->CopyData (&raw_data[datasize], cell->GetSize ());

        // check if just packaged
        auto opp_con = circ->GetOppositeConnection(direction);
        if (!opp_con->SpeaksCells() && DynamicCast<PseudoServerSocket>(opp_con->GetSocket()))
        {
          // notify about bytes entering the network
          torapp->m_triggerBytesEnteredNetwork(torapp, circid, data_index_last_seen[circid] + 1, data_index_last_seen[circid] + cell_size);
        }

        datasize += cell_size;
        flushed_some = true;

        data_index_last_seen[circid] += cell_size;
        NS_LOG_LOGIC ("[" << torapp->GetNodeName () << ": connection " << GetRemoteName () << "] Actually sending one cell from circuit " << circ->GetId ());
      }

      SetActiveCircuits (circ->GetNextCirc (this));

      if (GetActiveCircuits () == start_circ)
        {
          if (!flushed_some)
            {
              break;
            }
          flushed_some = false;
        }
    }

  // send data
  max_write = min (max_write, datasize);
  if (max_write > 0)
    {
      written_bytes = m_socket->Send (raw_data, max_write, 0);
      NS_LOG_LOGIC ("[" << torapp->GetNodeName () << ": connection " << GetRemoteName () << "] " << (int) written_bytes << " bytes actually written");
    }
  NS_ASSERT(written_bytes >= 0);
  NS_ASSERT(written_bytes == (int)max_write); // because, before calling Write(), this is capped at GetTxAvailable()

  /* save leftover for next time */
  written_bytes = max (written_bytes,0);
  uint32_t leftover = datasize - written_bytes;
  memcpy (outbuf.data, &raw_data[datasize - leftover], leftover);
  outbuf.size = leftover;

  if(leftover > 0)
    NS_LOG_LOGIC ("[" << torapp->GetNodeName () << ": connection " << GetRemoteName () << "] " << leftover << " bytes left over for next conn write");

  return written_bytes;
}

void
PredConnection::SendConnLevelCell (Ptr<Packet> cell)
{
  if (!SpeaksCells())
  {
    NS_LOG_LOGIC ("[" << torapp->GetNodeName() << ": connection " << GetRemoteName () << "] Dropping connection-level cell before sending because the remote is not a relay");
    return;
  }

  if (!m_controlsocket)
  {
    NS_LOG_LOGIC ("[" << torapp->GetNodeName() << ": connection " << GetRemoteName () << "] Dropping connection-level cell before sending because the control connection has not (yet) been established");
    return;
  }

  int sent_bytes = m_controlsocket->Send(cell);

  // We do not handle a full transmission buffer here, this is expected
  // not to happen (might fail if feedback messages grow).
  NS_ASSERT(sent_bytes == (int) cell->GetSize());
}

void
PredConnection::BeamConnLevelCell (Ptr<Packet> cell)
{
  if (!SpeaksCells())
  {
    return;
  }

  // Do not beam cells before the (inband) control connection has been established.
  // This is not really necessary at all, but is done to ensure that the feedback
  // messages exchanged are the same between different runs with inband and
  // out-of-band feedback.
  if (!m_controlsocket)
  {
    NS_LOG_LOGIC ("[" << torapp->GetNodeName() << ": connection " << GetRemoteName () << "] Dropping connection-level cell before sending because the control connection has not (yet) been established");
    return;
  }

  Ptr<UniformRandomVariable> rng = CreateObject<UniformRandomVariable> ();
  if (rng->GetValue() < torapp->GetFeedbackLoss())
  {
    NS_LOG_LOGIC ("[" << torapp->GetNodeName() << ": connection " << GetRemoteName () << "] Dropping feedback message randomly.");
    return;
  }
  
  auto remote_conn = GetRemoteConn ();
  if (!remote_conn)
  {
    NS_LOG_LOGIC ("[" << torapp->GetNodeName() << ": connection " << GetRemoteName () << "] Cannot beam cell (yet). Dropping it.");
    return;
  }
  Time delay = GetBaseRtt () / 2.0 - MilliSeconds(2); // TODO
  
  NS_LOG_LOGIC ("[" << torapp->GetNodeName() << ": connection " << GetRemoteName () << "] Beaming connection-level cell to appear after " << delay.GetSeconds() << " seconds");

  dumper.dump("sent-feedback-message",
              "time", Simulator::Now().GetSeconds(),
              "from-relay", torapp->GetNodeName(),
              "to-relay", GetRemoteConn()->GetTorApp()->GetNodeName(),
              "conn", GetRemoteName(),
              "bytes", (int)(cell->GetSize()));

  Simulator::Schedule(delay, &PredConnection::ReceiveBeamedConnLevelCell, remote_conn, cell);
}

void
PredConnection::ReceiveBeamedConnLevelCell(Ptr<Packet> cell)
{
  torapp->controller->HandleFeedback (this, cell);
}

void
PredConnection::ScheduleWrite (void)
{
  if (m_socket && write_event.IsExpired ())
    {
      write_event = Simulator::Schedule (Seconds(0), &TorPredApp::ConnWriteCallback, torapp, m_socket, m_socket->GetTxAvailable ());
    }
}

void
PredConnection::ScheduleWriteWrapper (int64_t prev_write_bucket)
{
  ScheduleWrite ();
}

void
PredConnection::ScheduleRead (Time delay)
{
  if (m_socket && read_event.IsExpired ())
    {
      read_event = Simulator::Schedule (delay, &TorPredApp::ConnReadCallback, torapp, m_socket);
    }
}


uint32_t
PredConnection::GetOutbufSize ()
{
  return outbuf.size;
}

uint32_t
PredConnection::GetInbufSize ()
{
  return inbuf.size;
}


//
// The optimization-based controller of PredicTor.
//

void
PredController::AddInputConnection (Ptr<PredConnection> conn)
{
  in_conns.insert(conn);
}

void
PredController::AddOutputConnection (Ptr<PredConnection> conn)
{
  out_conns.insert(conn);
}

void
PredController::DumpConnNames()
{
  int conn_index = 0;

  for (auto& conn : out_conns)
  {
    // remember the name of the out conn
    dumper.dump("outconn-name",
                "node", app->GetNodeName(),
                "index", conn_index,
                "name", conn->GetRemoteName());
    conn_index++;
  }
}

void
PredController::Setup ()
{
  // Get the maximum data rate
  DataRateValue datarate_app;
  app->GetAttribute("BandwidthRate", datarate_app);

  DataRateValue datarate_dev;
  app->GetNode()->GetDevice(0)->GetObject<PointToPointNetDevice>()->GetAttribute("DataRate", datarate_dev);

  max_datarate = std::min (datarate_app.Get(), datarate_dev.Get());

  dumper.dump("max-datarate",
              "time", Simulator::Now().GetSeconds(),
              "node", app->GetNodeName(),
              "pps", to_packets_sec(max_datarate)
  );

  // collect the input connections...
  vector<vector<uint16_t>> inputs;

  for (auto it = in_conns.begin (); it != in_conns.end(); ++it)
  {
    // ...and their circuits
    auto conn = *it;
    vector<uint16_t> circ_ids;

    for (auto& circ : conn->GetAllActiveCircuits())
    {
      circ_ids.push_back(circ->GetId());
    }

    inputs.push_back(circ_ids);
  }

  vector<string> input_types;
  for (auto& conn : in_conns)
  {
    if (!conn->SpeaksCells())
      input_types.push_back("exit");
    else
      input_types.push_back("node");
  }

  // collect the output connections...
  vector<vector<uint16_t>> outputs;
  num_circuits = 0;

  for (auto it = out_conns.begin (); it != out_conns.end(); ++it)
  {
    // ...and their circuits
    auto conn = *it;
    vector<uint16_t> circ_ids;

    for (auto& circ : conn->GetAllActiveCircuits())
    {
      circ_ids.push_back(circ->GetId());
    }

    outputs.push_back(circ_ids);
    num_circuits += circ_ids.size();
  }

  // auto obj = 
  auto result = pyscript.call(
    "setup",
    "time", Simulator::Now().GetSeconds(),
    "relay", app->GetNodeName (),
    "v_in_max_total", .85*to_packets_sec(MaxDataRate ()),  // TODO
    "v_out_max_total", .85*to_packets_sec(MaxDataRate ()), // TODO
    "s_c_max_total", 100,
    "scaling", 50,
    "dt", TimeStep().GetSeconds(),
    "N_steps", (int) Horizon(),
    "n_in", inputs.size(),
    "input_circuits", inputs,
    "n_out", outputs.size(),
    "output_circuits", outputs,
    "input_type", input_types
  );

  // Initialize some trajectories to be set by later optimization.
  // These are not needed, but the indices need to exist.
  for (size_t i=0; i<in_conns.size(); i++)
  {
    pred_v_out_source.push_back(Trajectory{this, Simulator::Now()});
    pred_s_buffer_source.push_back(Trajectory{this, Simulator::Now()});
  }

  for (size_t i=0; i<out_conns.size(); i++)
  {
    pred_v_out_max.push_back(Trajectory{this, Simulator::Now()});
  }

  // Set up the per-connection token buckets, initially assuming no restrictions
  // on the sending rate (v_out).

  Ptr<UniformRandomVariable> rng = CreateObject<UniformRandomVariable> ();
  rng->SetAttribute ("Min", DoubleValue (0.0));
  rng->SetAttribute ("Max", DoubleValue (0.001));

  for (auto& conn : out_conns)
  {
    conn_buckets[conn] = std::numeric_limits<uint64_t>::max();

    conn_last_sent[conn] = 0;
  }

  for (auto& conn : in_conns)
  {
    conn_read_buckets[conn] = std::numeric_limits<uint64_t>::max();
    conn_last_received[conn] = 0;

    // Initialize the circuit-level counters
    for (auto& circ : conn->GetAllActiveCircuits())
    {
      circ_last_received[circ] = 0;
    }
  }

  // Schedule the first optimizer event
  optimize_event = Simulator::Schedule(Seconds(0.1), &PredController::Optimize, this);

  // Schedule logging the names of the connections (the other applications have
  // to run their ::StartApplication() first).
  Simulator::ScheduleNow(&PredController::DumpConnNames, this);

  // Since the batched executor does only return a plain pointer to the parsed
  // JSON doc, we need to free it here.
  delete result;

  // TODO: verify success
}

void
PredController::Start ()
{
  // NS_LOG_LOGIC ("[" << app->GetNodeName() << "] starting the optimizer.");

  // // Schedule the first optimizer event
  // optimize_event = Simulator::ScheduleNow(&PredController::Optimize, this);
}

void
PredController::Optimize ()
{
  // Before doing anything else, measure and log how much data we have sent
  // and received during the last time step
  for (auto&& conn : out_conns)
  {
    auto last = conn_last_sent[conn];
    auto current = conn->GetDataSent();

    dumper.dump("data-sent-in-timestep",
                "time-start", Simulator::Now().GetSeconds() - TimeStep().GetSeconds(),
                "time-end", Simulator::Now().GetSeconds(),
                "node", app->GetNodeName(),
                "conn", conn->GetRemoteName(),
                "bytes", (int)(current-last)
    );
    
    // remeber the current value
    conn_last_sent[conn] = current;
  }

  for (auto&& conn : in_conns)
  {
    auto last = conn_last_received[conn];
    auto current = conn->GetDataReceived();

    dumper.dump("data-received-in-timestep",
                "time-start", Simulator::Now().GetSeconds() - TimeStep().GetSeconds(),
                "time-end", Simulator::Now().GetSeconds(),
                "node", app->GetNodeName(),
                "conn", conn->GetRemoteName(),
                "bytes", (int)(current-last)
    );

    // remeber the current value
    conn_last_received[conn] = current;

    // do the same for each of the circuits
    int circ_index = 0;

    // Initialize the circuit-level counters
    for (auto& circ : conn->GetAllActiveCircuits())
    {
      auto circ_last = circ_last_received[circ];
      auto circ_current = (uint64_t) circ->GetBytesRead(circ->GetOppositeDirection(conn));

      dumper.dump("circuit-data-received-in-timestep",
                  "time-start", Simulator::Now().GetSeconds() - TimeStep().GetSeconds(),
                  "time-end", Simulator::Now().GetSeconds(),
                  "node", app->GetNodeName(),
                  "conn", conn->GetRemoteName(),
                  "local-circuit-index", circ_index,
                  "circuit-id", (int) circ->GetId(),
                  "bytes", (int)(circ_current-circ_last)
      );
      circ_last_received[circ] = circ_current;
      circ_index++;
    }
  }

  // Firstly, measure the necessary local information.

  // Get the connections' queue lengthes
  vector<double> packets_per_conn;

  for (auto&& conn : out_conns)
  {
    double this_conn = 0.0;

    for (auto& circ : conn->GetAllActiveCircuits())
    {
      double queue_size = circ->GetQueueSize(circ->GetDirection(conn));
      this_conn += queue_size;
    }
    this_conn += (double)conn->GetOutbufSize() / (double)CELL_NETWORK_SIZE;

    packets_per_conn.push_back(this_conn);
  }

  //
  // Collect the trajectories necessary for optimizing
  //

  // data we are expected to receive from our predecessors (their v_out)
  vector<Trajectory> v_out_source;

  {
    // Use the following "idle" trajectory if we do not yet have data from other relays,
    // assuming that they do not send anything
    Trajectory idle{this, Simulator::Now()};
    for (unsigned int i=0; i<Horizon(); i++)
    {
      idle.Elements().push_back(0.0);
    }

    auto conn_it = in_conns.begin();
    int inconn_index = 0;
    for (auto&& req : pred_v_out_source)
    {
      NS_ASSERT(conn_it != in_conns.end());
      auto conn = *conn_it;
      
      if (req.Steps() == 0)
      {
        // Firstly, determine if this is a server edge
        if (!conn->SpeaksCells())
        {
          // This is a sending exit server socket. If it is sending already,
          // it will do so at the speed of our v_in, because this is what
          // we used to throttle the reading from the pseudo server socket.
          auto serversock = DynamicCast<PseudoServerSocket>(conn->GetSocket());
          NS_ASSERT(serversock);
          if (serversock->HasStarted())
          {
            // We have to take into account the amount of data that is still
            // available in the socket and "cap" v_in accordingly so it becomes
            // idle when the socket is emptied.
            Trajectory capped_v_in{this, Simulator::Now()};
            double current_buffer = serversock->GetRxAvailable() / CELL_NETWORK_SIZE;

            for (double rate : pred_v_in[inconn_index].Elements())
            {
              double consumed_packets = rate * TimeStep().GetSeconds();

              if (consumed_packets <= current_buffer) {
                capped_v_in.Elements().push_back(rate);
              }
              else {
                // Buffer does not suffice to realize this rate.
                // Reduce the rate such that it only uses the available buffer
                rate = current_buffer / TimeStep().GetSeconds();
                consumed_packets = rate * TimeStep().GetSeconds();
                capped_v_in.Elements().push_back(rate);
              }
              current_buffer = std::max(0.0, current_buffer-consumed_packets);
            }
            v_out_source.push_back(capped_v_in);
          }
          else
          {
            v_out_source.push_back(idle);
          }
        }
        else
        {
          // Otherwise, the predecessor has not requested to send anything,
          // assume it is idle.
          v_out_source.push_back(idle);
        }
      }
      else
      {
        req.DiscardUntil(Simulator::Now());
        NS_ASSERT (is_same_time(req.GetTime(), Simulator::Now()));
        v_out_source.push_back(req);
      }

      conn_it++;
      inconn_index++;
    }
  }

  // maximum outgoing data rate we were given by the successor
  vector<Trajectory> v_out_max;

  {
    // Use the following default trajectory if we do not yet have data from other relays,
    // assuming that we can can go full blast
    Trajectory unlimited{this, Simulator::Now()};
    for (unsigned int i=0; i<Horizon(); i++)
    {
      unlimited.Elements().push_back(to_packets_sec(MaxDataRate ()));
    }

    for (auto&& val : pred_v_out_max)
    {
      if (val.Steps() == 0)
      {
        v_out_max.push_back(unlimited);
      }
      else
      {
        val.DiscardUntil(Simulator::Now());
        NS_ASSERT (is_same_time(val.GetTime(), Simulator::Now()));
        v_out_max.push_back(val);
      }
    }
  }

  // future development of the predecessors' buffers
  vector<Trajectory> s_buffer_source;

  {
    // Use the following "idle" trajectory if we do not yet have data from other relays,
    // assuming that they are idle anything
    Trajectory idle{this, Simulator::Now()};
    for (unsigned int i=0; i<Horizon(); i++)
    {
      idle.Elements().push_back(0.0);
    }

    auto conn_it = in_conns.begin();
    int inconn_index = 0;
    for (auto&& val : pred_s_buffer_source)
    {
      NS_ASSERT(conn_it != in_conns.end());
      auto conn = *conn_it;
      
      if (val.Steps() == 0)
      {
        // Firstly, determine if this is a server edge
        if (!conn->SpeaksCells())
        {
          // This is a sending exit server socket. If it is sending already,
          // assume that it has a virtually infinite amount of data remaining.
          auto serversock = DynamicCast<PseudoServerSocket>(conn->GetSocket());
          NS_ASSERT(serversock);
          if (serversock->HasStarted())
          {
            double infinite = 60*to_packets_sec(MaxDataRate ());
            Trajectory socket_bytes_remaining {this, Simulator::Now()};
            double last_buffer_size;
            {
              double candidate = (double)serversock->GetRxAvailable() / CELL_NETWORK_SIZE;
              last_buffer_size = candidate;
              candidate = std::min(infinite, candidate);
              if (candidate >= 1.0/CELL_NETWORK_SIZE && candidate <= 1.0)
              {
                candidate = ceil(candidate);
              }
              socket_bytes_remaining.Elements().push_back(candidate);
            }

            for (unsigned int i=1; i<Horizon(); i++)
            {
              if (v_out_source[inconn_index].Elements().size() > i-1) {
                double prediction = last_buffer_size - v_out_source[inconn_index].Elements()[i-1]*TimeStep().GetSeconds();
                double candidate = std::min(std::max(prediction, 0.0), infinite);
                if (candidate >= 1.0/CELL_NETWORK_SIZE && candidate <= 1.0)
                {
                  candidate = ceil(candidate);
                }
                socket_bytes_remaining.Elements().push_back(candidate);

                last_buffer_size -= v_out_source[inconn_index].Elements()[i-1]*TimeStep().GetSeconds();
              }
              else
              {
                socket_bytes_remaining.Elements().push_back(0.0);
              }
            }

            s_buffer_source.push_back(socket_bytes_remaining);
          }
          else
          {
            s_buffer_source.push_back(idle);
          }
        }
        else
        {
          // Otherwise, the predecessor has not told us about its buffers yet,
          // assume it is idle.
          s_buffer_source.push_back(idle);
        }
      }
      else
      {
        val.DiscardUntil(Simulator::Now());
        NS_ASSERT (is_same_time(val.GetTime(), Simulator::Now()));
        s_buffer_source.push_back(val);
      }

      conn_it++;
      inconn_index++;
    }
  }

  // Dump optimization result
  dumper.dump("measured-s-buffer-0",
              "time", Simulator::Now().GetSeconds(),
              "node", app->GetNodeName(),
              "packets_per_conn", packets_per_conn
  );

  double control_delta = 0.0;

  // dump the call
  pyscript.dump(
    "calling-optimizer",
    "time", Simulator::Now().GetSeconds(),
    "relay", app->GetNodeName (),
    "s_buffer_0", packets_per_conn,

    // trajectories
    "v_out_max", transpose_to_double_vectors(v_out_max),
    "s_buffer_source", transpose_to_double_vectors(s_buffer_source),
    "v_out_source", transpose_to_double_vectors(v_out_source),
    "control_delta", control_delta
  );

  // Call the optimizer
  executor.Compute([=]
    {
      return pyscript.call(
        "solve",
        "time", Simulator::Now().GetSeconds(),
        "relay", app->GetNodeName (),
        "s_buffer_0", packets_per_conn,

        // trajectories
        "v_out_max", transpose_to_double_vectors(v_out_max),
        "s_buffer_source", transpose_to_double_vectors(s_buffer_source),
        "v_out_source", transpose_to_double_vectors(v_out_source),
        "control_delta", control_delta
      );
    },
    MakeCallback(&PredController::OptimizeDone, this)
  );

  // Schedule the next optimization step
  optimize_event = Simulator::Schedule(TimeStep (), &PredController::Optimize, this);
}

double
PredController::to_packets_sec(DataRate rate)
{
  return rate.GetBitRate() / (8.0*double{CELL_NETWORK_SIZE});
}

DataRate
PredController::to_datarate(double pps)
{
  return DataRate { uint64_t(pps*8*CELL_NETWORK_SIZE) };
}

vector<vector<double>>
PredController::to_double_vectors(vector<Trajectory> trajectories)
{
  vector<vector<double>> result;
  for (auto&& traj : trajectories)
  {
    result.push_back(traj.Elements());
  }
  return result;
}

vector<vector<double>>
PredController::transpose_to_double_vectors(vector<Trajectory> trajectories)
{
  vector<vector<double>> result;

  NS_ASSERT (trajectories.size() > 0);
  
  const int horizon = trajectories[0].Steps();

  for (int i=0; i<horizon; i++)
  {
    vector<double> this_step;

    for (auto&& traj : trajectories)
    {
      this_step.push_back(traj.Elements().at(i));
    }

    result.push_back(this_step);
  }

  return result;
}

void
PredController::OptimizeDone(rapidjson::Document * doc)
{
  Time now = Simulator::Now();
  Time next_step = now + TimeStep();

  // Save the raw optimizer response
  {
    rapidjson::StringBuffer buffer;
    buffer.Clear();
    rapidjson::Writer<rapidjson::StringBuffer> writer(buffer);
    doc->Accept(writer);

    dumper.dump("optimizer-returned",
                "time", Simulator::Now().GetSeconds(),
                "node", app->GetNodeName(),
                "response-json", buffer.GetString()
    );
  }

  ParseIntoTrajectories((*doc)["v_in"], pred_v_in, now, in_conns.size());
  ParseIntoTrajectories((*doc)["v_out"], pred_v_out, now, out_conns.size());
  ParseIntoTrajectories((*doc)["v_out_max"], pred_v_out_max, now, out_conns.size());
  ParseIntoTrajectories((*doc)["s_buffer"], pred_s_buffer, now, out_conns.size(), 1);

  // Clean slightly negative values
  for(auto& traj : pred_v_out)
  {
    traj.MakeNonNegativeChecked();
  }

  DumpMemoryPrediction();
  DumpVinPrediction();

  // Trigger sending of new information to peers
  SendToNeighbors();

  CalculateSendPlan();
  CalculateReadPlan();

  // Since the batched executor does only return a plain pointer to the parsed
  // JSON doc, we need to free it here.
  delete doc;
}

void
PredController::CalculateSendPlan()
{
  // Transfer the calculated send plan (v_out from the optimization step)
  // into ready-to-execute token buckets per connection

  auto conn_it = out_conns.begin();
  int conn_index = 0;

  vector<double> dumped_rates;
  vector<vector<double>> dumped_rate_trajectories;

  for (auto& traj : pred_v_out)
  {
    NS_ASSERT(conn_it != out_conns.end());

    double rate_pps = traj.Elements().at(0);
    DataRate rate = to_datarate(rate_pps);
    dumped_rates.push_back(rate_pps);
    dumped_rate_trajectories.emplace_back();
    for (auto& v : traj.Elements())
    {
      dumped_rate_trajectories.back().push_back(v);
    }

    Ptr<PredConnection> conn = *conn_it;

    NS_ASSERT(conn_buckets.find(conn) != conn_buckets.end());

    uint64_t conn_bucket = (uint64_t)(ceil(rate*time_step)/8.0); // dividing by 8 converts to bytes
    conn_buckets[conn] = conn_bucket;

    // trigger sending of new data
    conn->ScheduleWrite ();

    ++conn_it;
    ++conn_index;
  }

  // Dump optimization result
  dumper.dump("calculated-plan",
              "time", Simulator::Now().GetSeconds(),
              "node", app->GetNodeName(),
              "conn_rates_pps", dumped_rates,
              "conn_rates_pps_traj", dumped_rate_trajectories
  );
}

void
PredController::CalculateReadPlan()
{
  // Based on the predicted v_in values, calculate reading plans that define
  // how much data we read (at the most) from each input connection.
  //
  // Note that these plans are only used by the exit relays when reading from
  // the pseudo server sockets so they do not grow indefinitely!

  auto conn_it = in_conns.begin();

  for (auto& traj : pred_v_in)
  {
    NS_ASSERT(conn_it != in_conns.end());

    double rate_pps = traj.Elements().at(0);
    DataRate rate = to_datarate(rate_pps);

    Ptr<PredConnection> conn = *conn_it;

    NS_ASSERT(conn_read_buckets.find(conn) != conn_read_buckets.end());

    conn_read_buckets[conn] = (uint64_t)(rate*time_step/8.0); // dividing by 8 converts to bytes

    // trigger receiving of new data
    conn->ScheduleRead ();

    ++conn_it;
  }
}

void
PredController::DumpMemoryPrediction()
{
  vector<vector<double>> dumped_trajectories;

  for (auto& traj : pred_s_buffer)
  {
    dumped_trajectories.push_back(traj.Elements());
  }

  // Dump optimization result
  dumper.dump("predicted-s-buffer",
              "time", Simulator::Now().GetSeconds(),
              "node", app->GetNodeName(),
              "packets_traj", dumped_trajectories
  );
}

void
PredController::ParseIntoTrajectories(const rapidjson::Value& array, vector<Trajectory>& target, Time first_time, size_t expected_traj, size_t skip_steps)
{
  NS_ASSERT(array.IsArray());
  NS_ASSERT(array.Size() > 0);

  NS_ASSERT(array[0].IsArray());
  const size_t num_traj = array[0].Size();
  NS_ASSERT(num_traj == expected_traj);

  target.clear();
  for (size_t i=0; i<num_traj; i++) {
    target.push_back(Trajectory{this, first_time});
  }

  for (size_t step=skip_steps; step<array.Size(); step++) {
    NS_ASSERT(array[step].IsArray());
    NS_ASSERT(array[step].Size() == num_traj);

    for (size_t traj=0; traj< num_traj; traj++) {

    //   const char* kTypeNames[] = 
    // { "Null", "False", "True", "Object", "Array", "String", "Number" };
    //   cout << kTypeNames[array[step][traj].GetType()] << endl;

      // make sure this is a one-element array...
      NS_ASSERT(array[step][traj].IsArray());
      NS_ASSERT(array[step][traj].Size() == 1);

      NS_ASSERT(array[step][traj][0].IsDouble());

      target[traj].Elements().push_back(array[step][traj][0].GetDouble());
    }
  }
}

void
PredController::MergeTrajectories(Trajectory& target, Trajectory& source)
{
  Time target_time = GetNextOptimizationTime();
  NS_LOG_LOGIC ("[" << app->GetNodeName() << "] (merging something different) time diff: " << (target_time-source.GetTime()).GetSeconds());

  // TODO: maybe take into account initial value of target
  target = source;
  // TODO: Do we really not need to interpolate here? Consider possibly varying
  // feedback propagation delays...
  target = source.InterpolateToTime(GetNextOptimizationTime());
}

void
PredController::MergeTrajectories(vector<Trajectory>& target, vector<Ptr<Trajectory>>& source)
{
  NS_ASSERT(target.size() == source.size());
  
  Time target_time = GetNextOptimizationTime();

  Time source_time;
  size_t source_length = 0;
  bool first = true;

  for (auto&& traj : source)
  {
    if (first)
    {
      source_time = traj->GetTime();
      source_length = traj->Steps();
      first = false;
    }
    else
    {
      NS_ASSERT(source_time == traj->GetTime());
      NS_ASSERT(source_length == traj->Steps());
    }
  }

  NS_LOG_LOGIC ("[" << app->GetNodeName() << "] (merging into cv_in) time diff: " << (target_time-source_time).GetSeconds());

  double steps = (target_time - source_time).GetSeconds() / TimeStep().GetSeconds();
  NS_ASSERT(std::abs(std::round(steps) - steps) < 0.001);
  NS_ASSERT(std::lround(steps) >= 0);
  // TODO: reassure we do not need to take the offset into account
  // size_t start_index = (size_t) std::lround(steps);
  size_t start_index = (size_t) 0;

  // NS_ASSERT(start_index < source_length);
  start_index = std::min(start_index, source_length-1);

  target.clear();
  for (size_t i=0; i<source.size(); i++)
  {
    NS_LOG_LOGIC ("[" << app->GetNodeName() << "] (merging into cv_in) pushing traj");
    target.push_back(Trajectory{this, target_time});
  }

  for (size_t i=start_index; i<source_length; i++)
  {
    NS_LOG_LOGIC ("[" << app->GetNodeName() << "] (merging into cv_in) pushing regular element");
    for (size_t j=0; j<source.size(); j++)
    {
      target[j].Elements().push_back(source[j]->Elements()[i]);
    }
  }

  // repeat the last element of each trajectory if necessary
  for (size_t i=(source_length-start_index); i<source_length; i++)
  {
    NS_LOG_LOGIC ("[" << app->GetNodeName() << "] (merging into cv_in) pushing dummy element");
    for (size_t j=0; j<source.size(); j++)
    {
      target[j].Elements().push_back(source[j]->Elements()[source[j]->Elements().size()-1]);
    }
  }

  for (auto& traj : target)
  {
    NS_ASSERT(traj.Steps() == source_length);
  }


  NS_ASSERT(target.size() == source.size());
}

void
PredController::HandleInputFeedback(Ptr<PredConnection> conn, Ptr<Packet> cell)
{
    NS_LOG_LOGIC ("[" << app->GetNodeName() << ": connection " << conn->GetRemoteName () << "] received feedback from predecessor (input)");

    PredFeedbackMessage msg{cell};
    size_t conn_index = GetInConnIndex(conn);

    Ptr<Trajectory> v_out = msg.Get(FeedbackTrajectoryKind::VOut);
    MergeTrajectories(pred_v_out_source[conn_index], *v_out);

    Ptr<Trajectory> s_buffer = msg.Get(FeedbackTrajectoryKind::SBuffer);
    MergeTrajectories(pred_s_buffer_source[conn_index], *s_buffer);
}

void
PredController::HandleOutputFeedback(Ptr<PredConnection> conn, Ptr<Packet> cell)
{
    NS_LOG_LOGIC ("[" << app->GetNodeName() << ": connection " << conn->GetRemoteName () << "] received feedback from successor (output)");

    PredFeedbackMessage msg{cell};
    size_t conn_index = GetOutConnIndex(conn);

    Ptr<Trajectory> v_in = msg.Get(FeedbackTrajectoryKind::VIn);
    MergeTrajectories(pred_v_out_max[conn_index], *v_in);
}

bool
PredController::IsInConn(Ptr<PredConnection> conn)
{
  for (auto&& candidate : in_conns)
  {
    if(candidate == conn)
      return true;
  }
  return false;
}

bool
PredController::IsOutConn(Ptr<PredConnection> conn)
{
  for (auto&& candidate : out_conns)
  {
    if(candidate == conn)
      return true;
  }
  return false;
}

size_t
PredController::GetInConnIndex(Ptr<PredConnection> conn)
{
  size_t index = 0;
  for (auto&& candidate : in_conns)
  {
    if(candidate == conn)
      return index;
    
    index++;
  }
  NS_ABORT_MSG("incoming connection unknown to controller");
}

size_t
PredController::GetOutConnIndex(Ptr<PredConnection> conn)
{
  size_t index = 0;
  for (auto&& candidate : out_conns)
  {
    if(candidate == conn)
      return index;
    
    index++;
  }
  NS_ABORT_MSG("outgoing connection unknown to controller");
}

void PredController::DumpVinPrediction()
{
  size_t conn_index;

  NS_ASSERT(in_conns.size() == pred_v_in.size());

  conn_index = 0;
  for (auto&& conn : in_conns)
  {
    PredFeedbackMessage msg;

    dumper.dump("calculated-vin",
                "time", Simulator::Now().GetSeconds(),
                "node", app->GetNodeName(),
                "conn", conn->GetRemoteName(),
                "vin-traj-pps", pred_v_in[conn_index].Elements()
    );

    conn_index++;
  }
}

void
PredController::SendToNeighbors()
{
  size_t conn_index;

  //
  // Send v_in to each predecessor, so she can set her v_out_max
  //

  NS_ASSERT(in_conns.size() == pred_v_in.size());
  
  // predecessor
  conn_index = 0;
  for (auto&& conn : in_conns)
  {
    PredFeedbackMessage msg;

    msg.Add(FeedbackTrajectoryKind::VIn, pred_v_in[conn_index], -1);

    // Assemble the trajectories
    Ptr<Packet> packet = msg.MakePacket();

    NS_LOG_LOGIC ("[" << app->GetNodeName() << ": connection " << conn->GetRemoteName () << "] Sending connection-level cell");

    if (app->OutOfBandFeedback())
    {
      conn->BeamConnLevelCell (packet);
    }
    else
    {
      for (auto && fragment : MultiCellEncoder::encode(packet))
      {
        NS_LOG_LOGIC ("[" << app->GetNodeName() << ": connection " << conn->GetRemoteName () << "] Sending connection-level cell fragment");
        conn->SendConnLevelCell(fragment);
      }
    }

    conn_index++;
  }
  
  // successor
  conn_index = 0;
  for (auto&& conn : out_conns)
  {
    PredFeedbackMessage msg;

    msg.Add(FeedbackTrajectoryKind::VOut, pred_v_out[conn_index], +1);
    msg.Add(FeedbackTrajectoryKind::SBuffer, pred_s_buffer[conn_index], +1);

    // Assemble the message
    Ptr<Packet> packet = msg.MakePacket();

    NS_LOG_LOGIC ("[" << app->GetNodeName() << ": connection " << conn->GetRemoteName () << "] Sending connection-level cell");

    if (app->OutOfBandFeedback())
    {
      conn->BeamConnLevelCell (packet);
    }
    else
    {
      for (auto && fragment : MultiCellEncoder::encode(packet))
      {
        NS_LOG_LOGIC ("[" << app->GetNodeName() << ": connection " << conn->GetRemoteName () << "] Sending connection-level cell fragment");
        conn->SendConnLevelCell(fragment);
      }
    }
    
    conn_index++;
  }
}

BatchedExecutor PredController::executor;

//
// Container for handling trajectory data
//

Trajectory Trajectory::InterpolateToTime (Time target_time)
{
  // TODO: If target time is after the current start time, do not "cut off"
  //       the beginning of our trajectory, but keep the overall sum equal.
  if (target_time == first_time)
  {
    // If no interpolation necessary, just copy this trajectory
    return Trajectory{*this};
  }

  Trajectory result{time_step, target_time};
  int num_steps = Steps();
  
  Time last_time = target_time;

  for (int i=0; i<num_steps; i++)
  {
    // Add one step
    result.elements.push_back(GetAverageValue(last_time, last_time + time_step));
    last_time = last_time + time_step;
  }

  return result;
}

void Trajectory::DiscardUntil (Time target_time)
{
  if (is_same_time(target_time, first_time))
  {
    return;
  }

  // Find the beginning element
  double steps = (target_time - first_time).GetSeconds() / time_step.GetSeconds();
  NS_ASSERT(std::abs(std::round(steps) - steps) < 0.001);
  NS_ASSERT(std::lround(steps) >= 0);
  size_t start_index = (size_t) std::lround(steps);

  NS_ASSERT(start_index < Steps());
  NS_ASSERT(start_index > 0);

  // Move elements to front
  for (size_t i=start_index; i < Steps(); i++)
  {
    Elements()[i-start_index] = Elements()[i];
  }

  // Repeat the last element
  for (size_t i=0; i < start_index; i++)
  {
    Elements()[Steps()-1-i] = Elements()[Steps()-1-start_index];
  }

  first_time = target_time;
}

string FormatFeedbackKind(FeedbackTrajectoryKind kind)
{
  switch(kind)
  {
    case FeedbackTrajectoryKind::VIn: return "VIn";
    case FeedbackTrajectoryKind::VOut: return "VOut";
    case FeedbackTrajectoryKind::SBuffer: return "SBuffer";
  }

  NS_ABORT_MSG("Unknown feedback trajectory type: " << (int) kind);
}

std::ostream & operator << (std::ostream & os, const FeedbackTrajectoryKind & kind)
{
  os << FormatFeedbackKind(kind);
  return os;
};

PyScript dumper{"/bin/cat"};

bool is_same_time(const ns3::Time& x, const ns3::Time& y)
{
  double diff = x.GetSeconds() - y.GetSeconds();
  return (diff > -0.001) && (diff < 0.001);
}

} //namespace ns3
