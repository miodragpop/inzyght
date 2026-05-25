/**
 * Inzyght WebSocket Client
 * Handles real-time updates via WebSocket
 */

class InzyightWebSocketClient {
    constructor(url = null) {
        // Use current window location for WebSocket URL if not provided
        if (!url) {
            const protocol = window.location.protocol === 'https:' ? 'wss:' : 'ws:';
            url = protocol + '//' + window.location.host + '/ws';
        }

        this.url = url;
        this.ws = null;
        this.reconnectAttempts = 0;
        this.maxReconnectAttempts = 10;
        this.reconnectDelay = 1000; // Start with 1 second
        this.maxReconnectDelay = 30000; // Max 30 seconds
        this.subscriptions = new Set();
        this.heartbeatInterval = null;
        this.connectionStatus = 'disconnected';

        // Callbacks
        this.onConnected = null;
        this.onDisconnected = null;
        this.onMessage = null;
        this.onNetworkUpdate = null;
        this.onBlockUpdate = null;
        this.onTransactionUpdate = null;
        this.onError = null;
    }

    connect() {
        console.log('WebSocket: Connecting to ' + this.url);

        try {
            this.ws = new WebSocket(this.url);
            this.ws.onopen = (event) => this.handleOpen(event);
            this.ws.onmessage = (event) => this.handleMessage(event);
            this.ws.onerror = (event) => this.handleError(event);
            this.ws.onclose = (event) => this.handleClose(event);
        } catch (error) {
            console.error('WebSocket: Connection error:', error);
            this.scheduleReconnect();
        }
    }

    disconnect() {
        console.log('WebSocket: Disconnecting');
        this.clearHeartbeat();

        if (this.ws) {
            this.ws.close();
            this.ws = null;
        }

        this.connectionStatus = 'disconnected';
        if (this.onDisconnected) {
            this.onDisconnected();
        }
    }

    handleOpen(event) {
        console.log('WebSocket: Connected');
        this.reconnectAttempts = 0;
        this.reconnectDelay = 1000;
        this.connectionStatus = 'connected';

        // Start heartbeat
        this.startHeartbeat();

        // After a reconnect, the server has no memory of our previous
        // subscriptions. Re-send the wire messages directly (bypassing the
        // idempotent subscribe() so we don't no-op on channels already in
        // our local Set). On a first connection this loop is a no-op
        // because the Set is empty.
        for (const channel of this.subscriptions) {
            this.send({ type: 'subscribe', channel: channel });
        }

        if (this.onConnected) {
            this.onConnected();
        }
    }

    handleMessage(event) {
        try {
            const message = JSON.parse(event.data);
            console.log('WebSocket: Message received', message.type);

            // Route message to appropriate handler
            if (message.type === 'network') {
                if (this.onNetworkUpdate) {
                    this.onNetworkUpdate(message.data);
                }
            } else if (message.type === 'block') {
                if (this.onBlockUpdate) {
                    this.onBlockUpdate(message.data);
                }
            } else if (message.type === 'blocks') {
                // Array of blocks
                if (this.onBlockUpdate) {
                    for (const block of message.data) {
                        this.onBlockUpdate(block);
                    }
                }
            } else if (message.type === 'transaction') {
                if (this.onTransactionUpdate) {
                    this.onTransactionUpdate(message.data);
                }
            } else if (message.type === 'transactions') {
                // Array of transactions
                if (this.onTransactionUpdate) {
                    for (const tx of message.data) {
                        this.onTransactionUpdate(tx);
                    }
                }
            } else if (message.type === 'pong') {
                console.log('WebSocket: Pong received');
            } else if (message.type === 'subscription_ack') {
                console.log('WebSocket: Subscription acknowledged for', message.channel);
            }

            if (this.onMessage) {
                this.onMessage(message);
            }
        } catch (error) {
            console.error('WebSocket: Error parsing message:', error);
        }
    }

    handleError(event) {
        console.error('WebSocket: Error', event);
        this.connectionStatus = 'error';

        if (this.onError) {
            this.onError(event);
        }
    }

    handleClose(event) {
        console.log('WebSocket: Connection closed');
        this.connectionStatus = 'disconnected';
        this.clearHeartbeat();

        if (this.onDisconnected) {
            this.onDisconnected();
        }

        // Attempt to reconnect
        this.scheduleReconnect();
    }

    scheduleReconnect() {
        if (this.reconnectAttempts < this.maxReconnectAttempts) {
            this.reconnectAttempts++;

            // Exponential backoff
            const delay = Math.min(
                this.reconnectDelay * Math.pow(1.5, this.reconnectAttempts - 1),
                this.maxReconnectDelay
            );

            console.log('WebSocket: Reconnecting in ' + Math.round(delay / 1000) + ' seconds');

            setTimeout(() => {
                if (this.connectionStatus === 'disconnected') {
                    this.connect();
                }
            }, delay);
        } else {
            console.error('WebSocket: Max reconnection attempts reached');
            if (this.onError) {
                this.onError('Max reconnection attempts reached');
            }
        }
    }

    subscribe(channel) {
        // Idempotent: only send the wire message if this is a new
        // subscription. Callers (e.g. onConnected handlers) can safely call
        // subscribe() on every connect without producing duplicate server
        // registrations.
        const isNew = !this.subscriptions.has(channel);
        this.subscriptions.add(channel);

        if (isNew && this.isConnected()) {
            this.send({ type: 'subscribe', channel: channel });
        }
    }

    unsubscribe(channel) {
        if (!this.subscriptions.has(channel)) {
            return;
        }
        this.subscriptions.delete(channel);

        if (this.isConnected()) {
            this.send({ type: 'unsubscribe', channel: channel });
        }
    }

    send(message) {
        if (this.isConnected()) {
            try {
                this.ws.send(JSON.stringify(message));
            } catch (error) {
                console.error('WebSocket: Send error:', error);
            }
        } else {
            console.warn('WebSocket: Not connected, cannot send message');
        }
    }

    startHeartbeat() {
        this.clearHeartbeat();

        // Send ping every 30 seconds
        this.heartbeatInterval = setInterval(() => {
            if (this.isConnected()) {
                this.send({ type: 'ping' });
            }
        }, 30000);
    }

    clearHeartbeat() {
        if (this.heartbeatInterval) {
            clearInterval(this.heartbeatInterval);
            this.heartbeatInterval = null;
        }
    }

    isConnected() {
        return this.ws && this.ws.readyState === WebSocket.OPEN;
    }

    getConnectionStatus() {
        return this.connectionStatus;
    }
}

// Global WebSocket client instance
let inzyightWS = null;

// Initialize WebSocket on document ready
document.addEventListener('DOMContentLoaded', function() {
    console.log('Initializing Inzyght WebSocket client');

    inzyightWS = new InzyightWebSocketClient();

    // Set up event handlers
    inzyightWS.onConnected = function() {
        console.log('WebSocket connected, subscribing to channels');
        inzyightWS.subscribe('network');
        inzyightWS.subscribe('blocks');
        inzyightWS.subscribe('transactions');

        stopPolling();
        updateConnectionStatus(true);
    };

    inzyightWS.onDisconnected = function() {
        console.log('WebSocket disconnected, falling back to polling');
        startPolling();
        updateConnectionStatus(false);
    };

    inzyightWS.onNetworkUpdate = function(data) {
        console.log('Network update received');
        updateStatistics(data);
    };

    inzyightWS.onBlockUpdate = function(data) {
        console.log('Block update received:', data && data.hash);
        // Small delay to allow the indexer to commit the block to DB before fetching
        setTimeout(function() {
            fetchLatestBlocks();
            fetchNetworkInfo();
        }, 1000);
    };

    inzyightWS.onTransactionUpdate = function(data) {
        console.log('Transaction update received');
        console.log('Transaction update received:', data && data.txid);
        fetchLatestTransactions();
    };

    inzyightWS.onError = function(error) {
        console.error('WebSocket error:', error);
        updateConnectionStatus(false);
    };

    // Connect WebSocket
    inzyightWS.connect();
});

/**
 * Update connection status indicator in UI
 */
function updateConnectionStatus(connected) {
    // You can add a visual indicator for connection status
    const statusClass = connected ? 'connected' : 'disconnected';
    console.log('Connection status changed to:', statusClass);
}
